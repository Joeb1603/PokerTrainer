#include "advisor/training_advisor.h"

#include <algorithm>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "range/hand_vs_range.h"
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

constexpr double kImpliedShare = 0.5;
constexpr double kImpliedPotCap = 0.5;
constexpr double kShoveMaxPots = 1.5;
constexpr int kPreflopShoveMaxBigBlinds = 15;
constexpr double kVeryStrongBar = 0.5;
constexpr double kSolidBar = 0.3;
constexpr double kChartPriorBigBlinds = 1.0;
constexpr double kChartPriorCallShare = 0.25;

// The chart bots' preflop sizes (preflop_size in gto_heuristic_engine.cpp).
constexpr double kOpenBigBlinds = 2.5;
constexpr int kIsoMultiple = 3;
constexpr double kFourBetMultiple = 2.3;

constexpr int kMinRangeRollouts = 100;
constexpr std::uint64_t kRangeSeedSalt = 0xD1B54A32D192ED03ULL;

std::size_t at(int index) { return static_cast<std::size_t>(index); }

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

bool contesting(const PublicSeat& s) {
    return s.position && (s.status == SeatStatus::Active || s.status == SeatStatus::AllIn);
}

// Preflop raise records and full raise levels before the decision (a short all-in
// is not a new level, as the engine and the range model rule; the first raise is).
struct PreflopLevels {
    int raises = 0;  // Every raise record, as the chart bots count them
    int levels = 0;
    int callers = 0;
    int last_full = 0;
};

PreflopLevels preflop_levels(const PlayerView& view) {
    PreflopLevels out;
    out.last_full = view.big_blind;
    int current = view.big_blind;
    for (const ActionRecord& a : view.actions) {
        if (a.street != Street::Preflop) continue;
        if (a.type == ActionType::Call) ++out.callers;
        if (a.type != ActionType::Bet && a.type != ActionType::Raise) continue;
        ++out.raises;
        const int increment = std::max(0, a.street_total - current);
        const bool full = increment >= out.last_full;
        if (full) out.last_full = increment;
        if (full || out.levels == 0) ++out.levels;
        current = std::max(current, a.street_total);
    }
    return out;
}

// The street total the chart bots would raise to here: preflop_size.
double standard_raise_to(const PlayerView& view, const PreflopLevels& pre) {
    const PublicSeat& me = view.seats[at(view.seat)];
    switch (pre.raises) {
        case 0: return view.big_blind * (kOpenBigBlinds + pre.callers);
        case 1: return static_cast<double>(view.current_bet) * (kIsoMultiple + pre.callers);
        case 2: return view.current_bet * kFourBetMultiple;
        default: return me.street_bet + me.stack;
    }
}

// Range equities for one decision: the per-combo vector against every opponent's
// range, and Monte Carlo results for sets of several opponents.
class RangeEquities {
public:
    RangeEquities(const PlayerView& view, std::array<std::optional<WeightedRange>, kMaxSeats> ranges,
                  const RangeEquityOptions& options)
        : view_(view), ranges_(std::move(ranges)), options_(options) {
        const CardMask dead = card_mask(std::span<const Card>(view.board)) |
                              card_mask(std::span<const Card>(*view.own_hole));
        std::bitset<kComboCount> needed;
        for (std::optional<WeightedRange>& range : ranges_) {
            if (!range) continue;
            // A range the model emptied would make every number undefined: read it as any hand.
            WeightedRange live = *range;
            live.remove_cards(dead);
            if (!(live.total() > 0.0)) {
                live = WeightedRange::uniform();
                live.remove_cards(dead);
            }
            range = live;
            needed |= combos_in(*range);
        }
        combos_ = hero_vs_each_combo(*view.own_hole, view.board, needed, options_);
    }

    const std::optional<WeightedRange>& range(int index) const { return ranges_[at(index)]; }
    const ComboEquities& combos() const { return combos_; }

    // Asks for the sets of several opponents in one Monte Carlo run. Call before equity().
    void prepare(std::span<const std::uint8_t> sets) {
        for (const std::uint8_t set : sets) {
            if (std::popcount(static_cast<unsigned>(set)) < 2) continue;
            if (std::find(multi_sets_.begin(), multi_sets_.end(), set) == multi_sets_.end()) multi_sets_.push_back(set);
        }
        if (multi_sets_.empty()) return;
        std::vector<LayerRequest> requests;
        for (const std::uint8_t set : multi_sets_) requests.push_back(LayerRequest{set});
        try {
            multi_ = equity_vs_ranges(*view_.own_hole, view_.board, ranges_, requests, options_).equity;
        } catch (const std::invalid_argument&) {
            // Ranges narrowed to a few colliding combos (a villain can hold the only
            // card another range needs) cannot be dealt together. Treat the villains
            // as independent instead: the share is the product of heads-up shares.
            multi_.clear();
            for (const std::uint8_t set : multi_sets_) {
                double product = 1.0;
                for (int i = 0; i < kMaxSeats; ++i) {
                    if (((set >> i) & 1u) != 0) product *= against(i);
                }
                multi_.push_back(product);
            }
        }
    }

    // Hero's share against the opponents whose bit is set (1 against nobody).
    double equity(std::uint8_t set) const {
        const int count = std::popcount(static_cast<unsigned>(set));
        if (count == 0) return 1.0;
        if (count == 1) return against(std::countr_zero(static_cast<unsigned>(set)));
        const auto found = std::find(multi_sets_.begin(), multi_sets_.end(), set);
        if (found == multi_sets_.end()) throw std::logic_error("RangeEquities: set not prepared");
        return multi_[static_cast<std::size_t>(found - multi_sets_.begin())];
    }

    double against(int index) const { return equity_vs_range(combos_, *ranges_[at(index)]).value_or(0.0); }

private:
    const PlayerView& view_;
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges_;
    RangeEquityOptions options_;
    ComboEquities combos_;
    std::vector<std::uint8_t> multi_sets_;
    std::vector<double> multi_;
};

std::uint8_t all_in_set(const PlayerView& view, std::uint8_t set) {
    std::uint8_t out = 0;
    for (int i = 0; i < kMaxSeats; ++i) {
        if (((set >> i) & 1u) != 0 && view.seats[at(i)].status == SeatStatus::AllIn) {
            out = static_cast<std::uint8_t>(out | (1u << i));
        }
    }
    return out;
}

// What one bet or raise needs, besides the view.
struct RaiseInputs {
    const RangeEquities* equities = nullptr;
    const ArchetypeParams* params = nullptr;
    const WeightedRange* hero_range = nullptr;  // As the opponents see it, before this action
    const StreetStrength* strength = nullptr;   // Postflop only
    double pot_kept_share = 1.0;
    PreflopLevels pre;
    double standard_to = 0.0;
    int players_dealt = 0;
    std::uint8_t limpers = 0;  // Players who called before any raise
};

struct RaiseValue {
    double ev = 0.0;
    BetOutcome outcome;
};

RaiseValue raise_value(const PlayerView& view, const RaiseInputs& in, int to) {
    const PublicSeat& me = view.seats[at(view.seat)];
    const bool preflop = view.street == Street::Preflop;

    // Only what somebody can match is really at risk.
    const int effective_to = clamp_raise_to(view, to);
    const double added = effective_to - me.street_bet;
    const double pot = view.total_pot;
    const int hero_total = me.street_bet + me.stack;
    const double facing = std::max(0, view.current_bet - me.street_bet);
    const double size = pot + facing > 0.0 ? (effective_to - view.current_bet) / (pot + facing) : 1.0;

    int levels_after = in.pre.levels;
    if (in.pre.levels == 0 || effective_to - view.current_bet >= in.pre.last_full) ++levels_after;
    const double size_ratio = in.standard_to > 0.0 ? effective_to / in.standard_to : 1.0;

    double fold_all = 1.0;
    double no_raise = 1.0;
    double call_weight = 0.0;
    double call_equity = 0.0;
    double call_match = 0.0;
    double raise_weight = 0.0;
    double raise_equity = 0.0;
    double raised_value = 0.0;
    int deepest_total = view.current_bet;

    for (int i = 0; i < kMaxSeats; ++i) {
        const PublicSeat& s = view.seats[at(i)];
        if (i == view.seat || s.status != SeatStatus::Active || !s.position) continue;
        const std::optional<WeightedRange>& range = in.equities->range(i);
        if (!range) continue;
        const int total = s.street_bet + s.stack;
        deepest_total = std::max(deepest_total, total);
        const double match = std::max(0, std::min(effective_to, total) - s.street_bet);

        Response answer;
        if (preflop) {
            const Position hero_position = view.position.value_or(Position::BTN);
            answer = predict_preflop_response(*range, *s.position, hero_position, levels_after, size_ratio,
                                              in.players_dealt, *in.params, ((in.limpers >> i) & 1u) != 0);
        } else {
            const double odds = match / (pot + added + match);
            answer = predict_postflop_response(*range, *in.strength, view.street, size, odds, *in.params,
                                               in.hero_range);
        }
        const double raise = std::clamp(answer.raise, 0.0, 1.0);
        const double call = std::clamp(answer.call, 0.0, 1.0 - raise);
        fold_all *= std::max(0.0, 1.0 - call - raise);
        no_raise *= 1.0 - raise;

        if (call > 0.0) {
            call_weight += call;
            call_equity += call * equity_vs_range(in.equities->combos(), answer.call_range).value_or(0.0);
            call_match += call * match;
        }
        if (raise > 0.0) {
            const double e = equity_vs_range(in.equities->combos(), answer.raise_range).value_or(0.0);
            // A pot-sized re-raise, capped by both stacks.
            const double reraise_to =
                std::max(static_cast<double>(effective_to),
                         std::min({effective_to + pot + added + match, static_cast<double>(hero_total),
                                   static_cast<double>(total)}));
            const double hero_cost = reraise_to - me.street_bet;
            const double raiser_cost = reraise_to - s.street_bet;
            raise_weight += raise;
            raise_equity += raise * e;
            raised_value += raise * std::max(-added, e * (pot + hero_cost + raiser_cost) - hero_cost);
        }
    }

    RaiseValue out;
    const double p_raise = 1.0 - no_raise;
    const double p_call = std::max(0.0, 1.0 - fold_all - p_raise);
    const double e_call = call_weight > 0.0 ? call_equity / call_weight : 0.0;
    const double m = call_weight > 0.0 ? call_match / call_weight : 0.0;
    const double v_raised = raise_weight > 0.0 ? raised_value / raise_weight : 0.0;

    double implied = 0.0;
    if (view.street != Street::River) {
        const int behind = std::min(hero_total, deepest_total) - effective_to;
        implied = std::min(std::max(0.0, e_call - 0.5) * std::max(0, behind) * kImpliedShare,
                           kImpliedPotCap * (pot + added + m));
    }
    const double v_called = e_call * (pot + added + m) - added + implied;
    out.ev = fold_all * pot * in.pot_kept_share + p_call * v_called + p_raise * v_raised;
    out.outcome.fold_pct = 100.0 * fold_all;
    out.outcome.raise_pct = 100.0 * p_raise;
    out.outcome.equity_called_pct = 100.0 * e_call;
    out.outcome.equity_raised_pct = raise_weight > 0.0 ? 100.0 * raise_equity / raise_weight : 0.0;
    return out;
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
    const std::array<Card, 2>& cards = *view.own_hole;
    const HandClass hand = HandClass::from_cards(cards[0], cards[1]);
    std::string text = hand.to_string() + " (" + std::string(to_string(preflop_tier(hand))) + " preflop tier";
    if (read.category) text += ", now " + describe_category(read.rank);
    if (read.strong_draw()) {
        text += read.flush_draw ? " with a flush draw" : " with an open-ended draw";
    } else if (read.gutshot) {
        text += " with a gutshot";
    }
    return text + ")";
}

}  // namespace

TrainingAdvisor::TrainingAdvisor(std::uint64_t seed, AdvisorOptions options) : seed_(seed), options_(options) {
    if (options_.rollouts <= 0 || options_.range_rollouts <= 0) {
        throw std::invalid_argument("TrainingAdvisor: rollouts must be positive");
    }
    if (options_.threads < 0 || options_.flop_rivers_per_turn < 0) {
        throw std::invalid_argument("TrainingAdvisor: threads and flop_rivers_per_turn must not be negative");
    }
}

TrainingAdvisor::TrainingAdvisor(std::uint64_t seed, int rollouts)
    : TrainingAdvisor(seed, AdvisorOptions{.rollouts = rollouts,
                                           .range_rollouts = std::max(kMinRangeRollouts, rollouts / 5 * 3)}) {}

HintRecommendation TrainingAdvisor::analyze(const PlayerView& view) const {
    if (!view.own_hole || view.legal.mask == 0) {
        throw std::invalid_argument("TrainingAdvisor::analyze: seat is not the one to act");
    }

    const std::array<Card, 2>& cards = *view.own_hole;
    const LegalActions& legal = view.legal;
    const PotMath math = pot_math(view);
    const int opponents = std::clamp(view.opponents_in_hand(), 1, kMaxOpponents);
    const std::uint64_t seed = decision_seed(seed_, view);
    const bool preflop = view.street == Street::Preflop;
    const ArchetypeParams& params = archetype_params(Archetype::Baseline);

    // Random-hand equity: the spec's raw equity, the log's `eq` and the size rule's input.
    EquityOptions options;
    options.rollouts = options_.rollouts;
    options.seed = seed;
    options.threads = options_.threads;
    EquityTable random_equities(cards, view.board, options);
    const double equity = random_equities.against(opponents);

    RangeEquityOptions range_options;
    range_options.seed = mix(seed ^ kRangeSeedSalt);
    range_options.flop_rivers_per_turn = options_.flop_rivers_per_turn;
    range_options.threads = options_.threads;
    range_options.multiway_rollouts = options_.range_rollouts;
    RangeEquities equities(view, infer_ranges(view, Archetype::Baseline), range_options);

    std::uint8_t everyone = 0;
    for (int i = 0; i < kMaxSeats; ++i) {
        if (i != view.seat && contesting(view.seats[at(i)]) && equities.range(i)) {
            everyone = static_cast<std::uint8_t>(everyone | (1u << i));
        }
    }
    const std::vector<PotLayer> layers = pot_layers(view);
    std::vector<std::uint8_t> sets{everyone};
    for (const PotLayer& layer : layers) {
        sets.push_back(layer.opponent_seats);
        sets.push_back(all_in_set(view, layer.opponent_seats));
    }
    equities.prepare(sets);

    double layered = 0.0;
    double kept = 0.0;
    int layer_chips = 0;
    for (const PotLayer& layer : layers) {
        layered += layer.amount * equities.equity(layer.opponent_seats);
        kept += layer.amount * equities.equity(all_in_set(view, layer.opponent_seats));
        layer_chips += layer.amount;
    }
    const double layered_equity = layer_chips > 0 ? layered / layer_chips : 0.0;
    const double range_equity = equities.equity(everyone);

    const bool may_raise = legal.allows(ActionType::Bet) || legal.allows(ActionType::Raise);

    RaiseInputs inputs;
    std::optional<StreetStrength> strength;
    WeightedRange hero_range;
    if (may_raise) {
        inputs.equities = &equities;
        inputs.params = &params;
        inputs.pot_kept_share = layer_chips > 0 ? kept / layer_chips : 1.0;
        inputs.pre = preflop_levels(view);
        inputs.standard_to = standard_raise_to(view, inputs.pre);
        for (int i = 0; i < kMaxSeats; ++i) {
            if (view.seats[at(i)].position) ++inputs.players_dealt;
        }
        if (preflop && inputs.pre.raises == 0) {
            for (const ActionRecord& a : view.actions) {
                if (a.type == ActionType::Call) inputs.limpers = static_cast<std::uint8_t>(inputs.limpers | (1u << a.seat));
            }
        }
        if (!preflop) {
            strength = street_strength(view.board);
            inputs.strength = &*strength;
            hero_range = infer_range_for(view, view.seat, Archetype::Baseline, false);
            if (!(hero_range.total() > 0.0)) {
                hero_range = WeightedRange::uniform();
                hero_range.remove_cards(card_mask(std::span<const Card>(view.board)));
            }
            inputs.hero_range = &hero_range;
        }
    }

    // Where the raise a chart wants is not legal, the call is that same choice.
    PreflopResponse chart_choice = preflop ? chart_response(view) : PreflopResponse::Fold;
    if (chart_choice == PreflopResponse::Raise && !may_raise) chart_choice = PreflopResponse::Call;

    // The chart prior. A free check costs nothing, so it is left alone. Rejected
    // options lose more the more they risk: the chart says most, and the all-in
    // equity of the model least, about the big ones. An approved call gains the
    // same, but an approved raise only the flat amount, or every chart raise would
    // become all in.
    const auto prior = [&](PreflopResponse option, int risked) {
        if (!preflop) return 0.0;
        const double flat = kChartPriorBigBlinds * view.big_blind;
        const double scaled = kChartPriorCallShare * risked;
        if (chart_choice != option) return -(flat + scaled);
        return option == PreflopResponse::Call ? flat + scaled : flat;
    };
    const auto raise_risk = [&](int to) { return clamp_raise_to(view, to) - view.seats[at(view.seat)].street_bet; };

    HintRecommendation hint;
    hint.equity_pct = equity * 100.0;
    hint.range_equity_pct = range_equity * 100.0;
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

    if (legal.allows(ActionType::Check)) {
        const double ev = layered_equity * math.contestable;
        hint.ev.check_call = ev;
        consider(ev, {ActionType::Check, 0}, "CHECK");
        hint.ev.fold = 0.0;  // Listed for completeness; a free card always beats it
    } else {
        hint.ev.fold = 0.0;
        consider(0.0, {ActionType::Fold, 0}, "FOLD");
        if (legal.allows(ActionType::Call)) {
            const double ev = layered_equity * (math.contestable + math.to_call) - math.to_call +
                              prior(PreflopResponse::Call, math.to_call);
            hint.ev.check_call = ev;
            consider(ev, {ActionType::Call, 0}, "CALL " + std::to_string(math.to_call));
        }
    }

    const BoardTexture texture = classify_board(view.board);
    const HandRead read = read_hand(cards, view.board);

    const ActionType raise_type = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
    std::array<std::string, kLadder.size()> rung_labels;
    std::optional<double> best_rung_ev;
    if (may_raise) {
        for (std::size_t i = 0; i < kLadder.size(); ++i) {
            const SizeRung& rung = kLadder[i];
            const int clean_to = raise_to_for_pot_fraction(view, rung.fraction);
            // Sized for what somebody can match, as raise_value values it.
            const int to = clamp_raise_to(view, clean_to);
            if (to >= legal.max_raise_to) continue;  // That is the all-in option below
            const bool repeated = std::any_of(kLadder.begin(), kLadder.begin() + static_cast<std::ptrdiff_t>(i),
                                              [&](const SizeRung& earlier) { return hint.*earlier.to == to; });
            if (repeated) continue;  // Sizes clamped to the same bet
            const RaiseValue value = raise_value(view, inputs, to);
            const double ev = value.ev + prior(PreflopResponse::Raise, raise_risk(to));
            hint.ev.*rung.ev = ev;
            hint.*rung.to = to;
            hint.outcomes[i] = value.outcome;
            rung_labels[i] = percent_of_pot_label(view, raise_type, to, clean_to, rung.fraction);
            best_rung_ev = std::max(best_rung_ev.value_or(ev), ev);
            if (preflop) consider(ev, {raise_type, to}, rung_labels[i]);
        }
    }

    const PublicSeat& me = view.seats[at(view.seat)];
    const std::string all_in_label = "ALL-IN " + std::to_string(me.street_bet + me.stack);
    if (legal.allows(ActionType::AllIn)) {
        // Without raising rights an all-in is just the call, already considered.
        if (may_raise) {
            const RaiseValue value = raise_value(view, inputs, legal.max_raise_to);
            hint.ev.all_in = value.ev + prior(PreflopResponse::Raise, raise_risk(legal.max_raise_to));
            hint.outcomes[kOutcomeAllIn] = value.outcome;
        } else {
            hint.ev.all_in = hint.ev.check_call.value_or(0.0);
        }
    }
    // Preflop, a single-street model overrates deep shoves with strong hands: the
    // callers are the strong part of each range, but a normal raise keeps the rest
    // in for later streets, which `implied` barely counts at a preflop pot's size.
    // So all-in is advised preflop only short (kPreflopShoveMaxBigBlinds effective),
    // within kShoveMaxPots of the pot, or as the only raise left.
    bool preflop_shove_ok = false;
    if (preflop && legal.allows(ActionType::AllIn)) {
        int deepest_total = 0;
        for (int i = 0; i < kMaxSeats; ++i) {
            const PublicSeat& s = view.seats[at(i)];
            if (i != view.seat && s.status == SeatStatus::Active) deepest_total = std::max(deepest_total, s.street_bet + s.stack);
        }
        const int effective = std::min(me.street_bet + me.stack, deepest_total);
        preflop_shove_ok = !may_raise || !best_rung_ev || effective <= kPreflopShoveMaxBigBlinds * view.big_blind ||
                           raise_risk(legal.max_raise_to) <= kShoveMaxPots * view.total_pot;
    }
    if (legal.allows(ActionType::AllIn) && (preflop_shove_ok || !may_raise)) {
        consider(*hint.ev.all_in, {ActionType::AllIn, 0}, all_in_label);
    }
    hint.base_ev = hint.ev;
    // A deep shove is not on offer, so it must not out-list the advice.
    if (preflop && legal.allows(ActionType::AllIn) && !preflop_shove_ok) hint.ev.all_in = std::min(*hint.ev.all_in, best_ev);

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

    char numbers[200];
    if (math.to_call > 0 && !math.facing_aggression) {
        std::snprintf(numbers, sizeof numbers,
                      "%.1f%% equity against the estimated range (%.1f%% against a random hand) versus %.1f%% pot odds",
                      hint.range_equity_pct, hint.equity_pct, hint.pot_odds_pct);
    } else if (math.to_call > 0) {
        std::snprintf(numbers, sizeof numbers,
                      "%.1f%% equity against the estimated range (%.1f%% against a random hand) versus %.1f%% pot "
                      "odds (MDF %.1f%%)",
                      hint.range_equity_pct, hint.equity_pct, hint.pot_odds_pct, hint.mdf_pct);
    } else {
        std::snprintf(numbers, sizeof numbers,
                      "%.1f%% equity against the estimated range (%.1f%% against a random hand) with nothing to call",
                      hint.range_equity_pct, hint.equity_pct);
    }
    // A fold above the price must say why, or the numbers appear to argue against it.
    std::string verdict = best_label + " has the highest estimated EV.";
    if (!size_reason.empty()) {
        verdict = "Betting beats the passive options, and " + best_label + " fits: " + size_reason + ".";
    } else if (best_action.type == ActionType::Fold && hint.equity_pct > hint.pot_odds_pct) {
        verdict = preflop ? "FOLD: the positional chart rejects this hand here, as raw equity overstates it."
                          : "FOLD: equity against random hands overstates this hand against the ranges that bet "
                            "and call here.";
    }
    hint.rationale = std::string(position) + " with " + hand_summary(view, read) + " on a " +
                     std::string(board_name == "no board" ? "preflop table with no board" : board_name) +
                     (board_name == "no board" ? "" : " board") + ": " + numbers + ". " + verdict;
    return hint;
}

}  // namespace poker
