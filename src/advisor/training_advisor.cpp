#include "advisor/training_advisor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "strategy/preflop_charts.h"

namespace poker {
namespace {

struct SizeRung {
    double fraction;
    std::optional<double> EvEstimates::*ev;
    std::optional<int> HintRecommendation::*to;
};
constexpr std::array<SizeRung, 4> kLadder{{{0.25, &EvEstimates::raise_25, &HintRecommendation::raise_25_to},
                                           {0.50, &EvEstimates::raise_50, &HintRecommendation::raise_50_to},
                                           {0.75, &EvEstimates::raise_75, &HintRecommendation::raise_75_to},
                                           {1.00, &EvEstimates::raise_100, &HintRecommendation::raise_100_to}}};

constexpr double kUnfacedCallFloor = 0.10;
constexpr double kMinimumCallFloor = 0.01;
constexpr double kImpliedShare = 0.5;
constexpr double kImpliedPotCap = 0.5;
constexpr double kShoveMaxPots = 1.5;
constexpr double kVeryStrongBar = 0.5;
constexpr double kSolidBar = 0.3;
constexpr double kUnopenedCallAtStandardOpen = 0.15;
constexpr double kStandardOpenBigBlinds = 2.5;
constexpr double kChartPriorBigBlinds = 1.0;
constexpr double kChartPriorCallShare = 0.25;

// splitmix64 finalizer: nearby inputs must not give related Monte Carlo streams.
std::uint64_t mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

std::uint64_t decision_seed(std::uint64_t seed, const PlayerView& view) {
    std::uint64_t hash = mix(seed);
    const auto add = [&hash](std::uint64_t value) { hash = mix(hash ^ value); };
    add(static_cast<std::uint64_t>(view.hand_number));
    add(view.actions.size());
    add((*view.own_hole)[0].id);
    add((*view.own_hole)[1].id);
    for (const Card card : view.board) add(card.id);
    return hash;
}

struct EvInputs {
    double equity = 0.0;
    double equity_when_called = 0.0;
    int foldable_opponents = 0;   // All-ins cannot fold
    double pot_kept_share = 1.0;  // Equity in the pot once everybody foldable has gone
    bool facing_bet = false;
    bool unopened_preflop = false;
};

int preflop_raises(const PlayerView& view) {
    return static_cast<int>(std::count_if(view.actions.begin(), view.actions.end(), [](const ActionRecord& a) {
        return a.street == Street::Preflop && a.type == ActionType::Raise;
    }));
}

double raise_ev(const PlayerView& view, const EvInputs& in, int to) {
    const PublicSeat& me = view.seats[view.seat];

    // Only what somebody can match is really at risk.
    const int effective_to = clamp_raise_to(view, to);
    const double added = effective_to - me.street_bet;

    int deepest_total = view.current_bet;
    int match = 0;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const PublicSeat& s = view.seats[seat];
        if (seat == view.seat || s.status != SeatStatus::Active) continue;
        const int total = s.street_bet + s.stack;
        deepest_total = std::max(deepest_total, total);
        match = std::max(match, std::min(effective_to, total) - s.street_bet);
    }

    const double pot = view.total_pot;
    const double s = pot > 0.0 ? added / pot : 1.0;
    const double mdf = 1.0 / (1.0 + s);
    const double floor = in.facing_bet ? std::min(0.60, 0.10 + 0.90 / s) : kUnfacedCallFloor;
    const double standard_open = kStandardOpenBigBlinds * view.big_blind;
    const double call_each = in.unopened_preflop
                                 ? std::max(kMinimumCallFloor, kUnopenedCallAtStandardOpen * standard_open / added)
                                 : std::max(mdf, floor);
    const double fold_all = std::pow(1.0 - call_each, in.foldable_opponents);

    const double exponent =
        (in.facing_bet || in.unopened_preflop ? 2.0 : 1.0) + fold_all + 0.25 * std::max(0.0, s - 1.0);
    const double called = std::pow(in.equity_when_called, exponent);

    double implied = 0.0;
    if (view.street != Street::River) {
        const int behind = std::min(me.street_bet + me.stack, deepest_total) - effective_to;
        implied = std::min(std::max(0.0, called - 0.5) * std::max(0, behind) * kImpliedShare,
                           kImpliedPotCap * (pot + added + match));
    }
    return fold_all * pot * in.pot_kept_share + (1.0 - fold_all) * (called * (pot + added + match) - added + implied);
}

struct SizeChoice {
    double fraction;
    const char* reason;
};

// The postflop size for a hand worth betting. The table is in training_advisor.h.
SizeChoice preferred_size(const BoardTexture& texture, const HandRead& read, std::span<const Card> board, Street street,
                          double equity, int opponents) {
    const auto bar = [opponents](double strength) {
        const double even_share = 1.0 / (opponents + 1);
        return even_share + (1.0 - even_share) * strength;
    };
    // Two pair on an unpaired board uses both hole cards, so it counts with sets.
    const bool made_big = is_nutted(read, board) || (read.category == HandCategory::TwoPair && !texture.paired);
    const bool very_strong = made_big && equity >= bar(kVeryStrongBar);
    const bool solid = very_strong || equity >= bar(kSolidBar);

    if (street == Street::River && very_strong) return {1.0, "a very strong river hand wants the biggest value bet"};
    if (texture.monotone) return {0.25, "a monotone board favors small bets"};
    if (texture.paired || !texture.wet) {
        return solid ? SizeChoice{0.5, "a standard value bet on a static board"}
                     : SizeChoice{0.25, "a cheap probe or thin value bet on a static board"};
    }
    if (very_strong && texture.dynamic) return {1.0, "heavy protection on a dynamic board"};
    if (solid || read.strong_draw()) return {0.75, "charges the draws on a wet board"};
    return {0.5, "medium protection for thin value on a wet board"};
}

std::string percent_of_pot_label(const PlayerView& view, ActionType type, int to, int clean_to, double fraction) {
    char buffer[64];
    const char* verb = type == ActionType::Bet ? "BET" : "RAISE to";
    if (to == clean_to) {
        std::snprintf(buffer, sizeof buffer, "%s %d (%d%% Pot)", verb, to, static_cast<int>(fraction * 100.0 + 0.5));
    } else if (to == view.legal.min_raise_to) {
        std::snprintf(buffer, sizeof buffer, "%s %d (minimum)", verb, to);
    } else {
        std::snprintf(buffer, sizeof buffer, "%s %d", verb, to);
    }
    return buffer;
}

std::string hand_summary(const PlayerView& view, const HandRead& read) {
    const std::array<Card, 2>& hole = *view.own_hole;
    std::string text = HandClass::from_cards(hole[0], hole[1]).to_string() + " (" +
                       std::string(to_string(preflop_tier(HandClass::from_cards(hole[0], hole[1])))) + " preflop tier";
    if (read.category) text += ", now " + describe_category(read.rank);
    if (read.strong_draw()) {
        text += read.flush_draw ? " with a flush draw" : " with an open-ended draw";
    } else if (read.gutshot) {
        text += " with a gutshot";
    }
    return text + ")";
}

}  // namespace

HintRecommendation TrainingAdvisor::analyze(const PlayerView& view) const {
    if (!view.own_hole || view.legal.mask == 0) {
        throw std::invalid_argument("TrainingAdvisor::analyze: seat is not the one to act");
    }

    const std::array<Card, 2>& hole = *view.own_hole;
    const LegalActions& legal = view.legal;
    const PotMath math = pot_math(view);
    const int opponents = std::clamp(view.opponents_in_hand(), 1, kMaxOpponents);
    const std::uint64_t seed = decision_seed(seed_, view);

    EquityOptions options;
    options.rollouts = rollouts_;
    options.seed = seed;
    EquityTable equities(hole, view.board, options);
    const double equity = equities.against(opponents);

    const bool preflop = view.street == Street::Preflop;
    const bool no_raise_yet = preflop && preflop_raises(view) == 0;

    int foldable = 0;
    bool limped = false;
    for (int other = 0; other < kMaxSeats; ++other) {
        if (other == view.seat || view.seats[other].status != SeatStatus::Active) continue;
        ++foldable;
        const auto limped_here = [other](const ActionRecord& a) { return a.seat == other && a.type == ActionType::Call; };
        limped = limped || (no_raise_yet && std::any_of(view.actions.begin(), view.actions.end(), limped_here));
    }
    const bool unopened = no_raise_yet && !limped;  // A limper has shown interest, so will not fold like a blind

    double called_base = equity;
    if (opponents > 1) {
        const double heads_up = equities.against(1);
        called_base = unopened ? heads_up : 0.5 * (equity + heads_up);
    }

    const std::vector<PotLayer> layers = pot_layers(view);
    double kept = 0.0;
    int layer_chips = 0;
    for (const PotLayer& layer : layers) {
        kept += layer.amount * equities.against(layer.all_in_opponents);
        layer_chips += layer.amount;
    }
    const EvInputs inputs{equity,
                          called_base,
                          foldable,
                          layer_chips > 0 ? kept / layer_chips : 1.0,
                          math.facing_bet > 0 && !no_raise_yet,
                          unopened};

    const bool may_raise = legal.allows(ActionType::Bet) || legal.allows(ActionType::Raise);
    // Where the raise a chart wants is not legal, the call is that same choice.
    PreflopResponse chart_choice = preflop ? chart_response(view) : PreflopResponse::Fold;
    if (chart_choice == PreflopResponse::Raise && !may_raise) chart_choice = PreflopResponse::Call;

    // The chart prior. A free check costs nothing, so it is left alone. Rejected
    // options lose more the more they risk: the chart says most, and random-hand
    // equity least, about the big ones. An approved call gains the same, but an
    // approved raise only the flat amount, or every chart raise would become all in.
    const auto prior = [&](PreflopResponse option, int risked) {
        if (!preflop) return 0.0;
        const double flat = kChartPriorBigBlinds * view.big_blind;
        const double scaled = kChartPriorCallShare * risked;
        if (chart_choice != option) return -(flat + scaled);
        return option == PreflopResponse::Call ? flat + scaled : flat;
    };
    const auto raise_risk = [&](int to) { return clamp_raise_to(view, to) - view.seats[view.seat].street_bet; };

    HintRecommendation hint;
    hint.equity_pct = equity * 100.0;
    hint.pot_odds_pct = math.pot_odds * 100.0;
    hint.mdf_pct = math.mdf * 100.0;
    hint.spr = math.spr;

    // Later options must beat earlier ones outright, so ties settle on the cheaper play.
    Action best_action{};
    std::string best_label;
    double best_ev = 0.0;
    bool have_best = false;
    const auto consider = [&](double ev, Action action, std::string label) {
        if (have_best && ev <= best_ev) return;
        best_ev = ev;
        best_action = action;
        best_label = std::move(label);
        have_best = true;
    };

    const double layered_equity = price_equity(layers, equities);

    if (legal.allows(ActionType::Check)) {
        const double ev = layered_equity * math.contestable;
        hint.ev.check_call = ev;
        consider(ev, {ActionType::Check, 0}, "CHECK");
        hint.ev.fold = 0.0;  // Listed for completeness; a free card always beats it
    } else {
        hint.ev.fold = 0.0;
        consider(0.0, {ActionType::Fold, 0}, "FOLD");
        if (legal.allows(ActionType::Call)) {
            const double facing_equity = layered_equity - bet_pressure(math, kGtoBetPressureDiscount);
            const double ev = facing_equity * (math.contestable + math.to_call) - math.to_call +
                              prior(PreflopResponse::Call, math.to_call);
            hint.ev.check_call = ev;
            consider(ev, {ActionType::Call, 0}, "CALL " + std::to_string(math.to_call));
        }
    }

    const BoardTexture texture = classify_board(view.board);
    const HandRead read = read_hand(hole, view.board);

    const ActionType raise_type = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
    std::array<std::string, kLadder.size()> rung_labels;
    std::optional<double> best_rung_ev;
    if (may_raise) {
        for (std::size_t i = 0; i < kLadder.size(); ++i) {
            const SizeRung& rung = kLadder[i];
            const int clean_to = raise_to_for_pot_fraction(view, rung.fraction);
            // Sized for what somebody can match, as raise_ev values it.
            const int to = clamp_raise_to(view, clean_to);
            if (to >= legal.max_raise_to) continue;  // That is the all-in option below
            const bool repeated = std::any_of(kLadder.begin(), kLadder.begin() + static_cast<std::ptrdiff_t>(i),
                                              [&](const SizeRung& earlier) { return hint.*earlier.to == to; });
            if (repeated) continue;  // Sizes clamped to the same bet
            const double ev = raise_ev(view, inputs, to) + prior(PreflopResponse::Raise, raise_risk(to));
            hint.ev.*rung.ev = ev;
            hint.*rung.to = to;
            rung_labels[i] = percent_of_pot_label(view, raise_type, to, clean_to, rung.fraction);
            best_rung_ev = std::max(best_rung_ev.value_or(ev), ev);
            if (preflop) consider(ev, {raise_type, to}, rung_labels[i]);
        }
    }

    const std::string all_in_label = "ALL-IN " + std::to_string(view.seats[view.seat].street_bet + view.seats[view.seat].stack);
    if (legal.allows(ActionType::AllIn)) {
        // Without raising rights an all-in is just the call, already considered.
        hint.ev.all_in = may_raise ? raise_ev(view, inputs, legal.max_raise_to) +
                                         prior(PreflopResponse::Raise, raise_risk(legal.max_raise_to))
                                   : hint.ev.check_call.value_or(0.0);
        if (preflop || !may_raise) consider(*hint.ev.all_in, {ActionType::AllIn, 0}, all_in_label);
    }
    hint.base_ev = hint.ev;

    std::string size_reason;
    if (!preflop && may_raise) {
        const bool can_shove = legal.allows(ActionType::AllIn);
        const bool shove_in_range =
            can_shove && (!best_rung_ev || raise_risk(legal.max_raise_to) <= kShoveMaxPots * view.total_pot);
        double best_bet = best_rung_ev.value_or(-std::numeric_limits<double>::infinity());
        if (shove_in_range) best_bet = std::max(best_bet, *hint.ev.all_in);

        if (best_bet > best_ev) {  // Betting beats the passive options, on the model alone
            const SizeChoice size =
                preferred_size(texture, read, view.board, view.street, equity, std::max(1, view.opponents_in_hand()));
            const int target = clamp_raise_to(view, raise_to_for_pot_fraction(view, size.fraction));
            const auto chosen = std::find_if(kLadder.begin(), kLadder.end(),
                                             [&](const SizeRung& rung) { return hint.*rung.to == target; });
            const bool shoves_on_ev = shove_in_range && (!best_rung_ev || *hint.ev.all_in > *best_rung_ev);
            if (shoves_on_ev || chosen == kLadder.end()) {
                // The chosen size would take the whole stack, or all-in is simply best.
                if (can_shove) {
                    hint.ev.all_in = best_bet;
                    consider(best_bet, {ActionType::AllIn, 0}, all_in_label);
                }
            } else {
                hint.ev.*chosen->ev = best_bet;
                consider(best_bet, {raise_type, target},
                         rung_labels[static_cast<std::size_t>(chosen - kLadder.begin())]);
                size_reason = size.reason;
            }
        }
        // A deep shove is not on offer, so it must not out-list the advice.
        if (can_shove && !shove_in_range) hint.ev.all_in = std::min(*hint.ev.all_in, best_ev);
    }

    hint.best_action = best_label;
    hint.action = best_action;

    const std::string_view position = view.position ? to_string(*view.position) : std::string_view("Unknown");
    const std::string_view board_name = texture_name(texture, !view.board.empty());

    char numbers[160];
    if (math.to_call > 0 && !math.facing_aggression) {
        std::snprintf(numbers, sizeof numbers, "%.1f%% equity against %.1f%% pot odds", hint.equity_pct,
                      hint.pot_odds_pct);
    } else if (math.to_call > 0) {
        std::snprintf(numbers, sizeof numbers, "%.1f%% equity against %.1f%% pot odds (MDF %.1f%%)", hint.equity_pct,
                      hint.pot_odds_pct, hint.mdf_pct);
    } else {
        std::snprintf(numbers, sizeof numbers, "%.1f%% equity with nothing to call", hint.equity_pct);
    }
    // A fold above the price must say why, or the numbers appear to argue against it.
    std::string verdict = best_label + " has the highest estimated EV.";
    if (!size_reason.empty()) {
        verdict = "Betting beats the passive options, and " + best_label + " fits: " + size_reason + ".";
    } else if (best_action.type == ActionType::Fold && hint.equity_pct > hint.pot_odds_pct) {
        verdict = preflop ? "FOLD: the positional chart rejects this hand here, as random-hand equity overstates it."
                          : "FOLD: equity against random hands overstates this hand against a betting range.";
    }
    hint.rationale = std::string(position) + " with " + hand_summary(view, read) + " on a " +
                     std::string(board_name == "no board" ? "preflop table with no board" : board_name) +
                     (board_name == "no board" ? "" : " board") + ": " + numbers + ". " + verdict;
    return hint;
}

}  // namespace poker
