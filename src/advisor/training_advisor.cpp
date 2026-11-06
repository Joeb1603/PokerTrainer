#include "advisor/training_advisor.h"

#include <algorithm>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
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
// The chart prior's weight in the archetype lines (training_advisor.h).
constexpr double kArchetypePriorScale = 1.0;

// The chart bots' preflop sizes (preflop_size in gto_heuristic_engine.cpp).
constexpr double kOpenBigBlinds = 2.5;
constexpr int kIsoMultiple = 3;
constexpr double kFourBetMultiple = 2.3;

constexpr int kMinRangeRollouts = 100;
// The archetype lines' multiway Monte Carlo: postflop, this many times fewer
// rollouts than Baseline's (with its seed, so the first chunk's deals are the
// same), and fewer still where narrow ranges make most deals collide, so
// that about kLineDealBudget deals are drawn in all (rejected ones included).
constexpr int kLineRolloutDivisor = 3;
constexpr double kLineDealBudget = 150000.0;
constexpr int kAcceptanceTrials = 512;
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

// Every range in `ranges` with the cards Hero can see removed. A range the model
// emptied would make every number undefined, so it is read as any hand. Returns
// the combos the ranges hold, which the per-combo vector must cover.
std::bitset<kComboCount> prepare_ranges(const PlayerView& view, std::array<std::optional<WeightedRange>, kMaxSeats>& ranges) {
    const CardMask dead = card_mask(std::span<const Card>(view.board)) | card_mask(std::span<const Card>(*view.own_hole));
    std::bitset<kComboCount> needed;
    for (std::optional<WeightedRange>& range : ranges) {
        if (!range) continue;
        WeightedRange live = *range;
        live.remove_cards(dead);
        if (!(live.total() > 0.0)) {
            live = WeightedRange::uniform();
            live.remove_cards(dead);
        }
        range = live;
        needed |= combos_in(*range);
    }
    return needed;
}

// The share of joint deals, one combo from each range by weight, in which no two
// opponents hold the same card: what equity_vs_ranges' redraws pay for. Estimated
// from kAcceptanceTrials deals with the given seed.
double deal_acceptance(const std::array<std::optional<WeightedRange>, kMaxSeats>& ranges, std::uint64_t seed) {
    std::vector<std::vector<double>> cumulative;
    for (const std::optional<WeightedRange>& range : ranges) {
        if (!range) continue;
        std::vector<double> sums(kComboCount);
        double running = 0.0;
        for (int combo = 0; combo < kComboCount; ++combo) {
            running += std::max(0.0f, range->weight(combo));
            sums[at(combo)] = running;
        }
        if (!(running > 0.0)) return 0.0;
        cumulative.push_back(std::move(sums));
    }
    if (cumulative.size() < 2) return 1.0;
    std::mt19937_64 rng(seed);
    int accepted = 0;
    for (int trial = 0; trial < kAcceptanceTrials; ++trial) {
        CardMask held = 0;
        bool clash = false;
        for (const std::vector<double>& sums : cumulative) {
            const double u = static_cast<double>(rng() >> 11) * 0x1.0p-53 * sums.back();
            const auto it = std::upper_bound(sums.begin(), sums.end(), u);
            const int combo = std::min(static_cast<int>(it - sums.begin()), kComboCount - 1);
            const CardMask mask = combo_mask(combo);
            clash = clash || (held & mask) != 0;
            held |= mask;
        }
        if (!clash) ++accepted;
    }
    return static_cast<double>(accepted) / kAcceptanceTrials;
}

// Range equities for one decision and one set of opponent ranges (prepared by
// prepare_ranges): the shared per-combo vector, and Monte Carlo results for sets
// of several opponents. Each entry of the vector depends only on its own combo,
// so one vector over the union of every line's ranges serves them all.
class RangeEquities {
public:
    RangeEquities(const PlayerView& view, std::array<std::optional<WeightedRange>, kMaxSeats> ranges,
                  const ComboEquities& combos, const RangeEquityOptions& options)
        : view_(view), ranges_(std::move(ranges)), options_(options), combos_(&combos) {}

    const std::optional<WeightedRange>& range(int index) const { return ranges_[at(index)]; }
    const ComboEquities& combos() const { return *combos_; }

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
            if (independent_) throw std::invalid_argument("dealing together is not tried");
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

    double against(int index) const { return equity_vs_range(*combos_, *ranges_[at(index)]).value_or(0.0); }

    // Skips the Monte Carlo for the fallback below: for ranges that a trial run
    // found could not be dealt together at all.
    void treat_as_independent() { independent_ = true; }

private:
    const PlayerView& view_;
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges_;
    RangeEquityOptions options_;
    const ComboEquities* combos_;
    std::vector<std::uint8_t> multi_sets_;
    std::vector<double> multi_;
    bool independent_ = false;
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
    // A solid or very strong hand by the rule's own bars: the value bets an
    // archetype line may resize. Probes, thin value and draws are not.
    bool value;
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

    if (street == Street::River && very_strong) return {1.0, "a very strong river hand wants the biggest value bet", true};
    if (texture.monotone) return {0.25, "a monotone board favors small bets", solid};
    if (texture.paired || !texture.wet) {
        return solid ? SizeChoice{0.5, "a standard value bet on a static board", true}
                     : SizeChoice{0.25, "a cheap probe or thin value bet on a static board", false};
    }
    if (very_strong && texture.dynamic) return {1.0, "heavy protection on a dynamic board", true};
    if (solid || read.strong_draw()) return {0.75, "charges the draws on a wet board", solid};
    return {0.5, "medium protection for thin value on a wet board", false};
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

// Runs task(i) for every i below count: on threads of its own when `parallel`
// (task 0 on the calling thread), else in order. Each task writes only its own
// slot, so the result is the same either way. The first failure, by index, is
// rethrown once every task has finished.
template <class Task>
void run_tasks(std::size_t count, bool parallel, const Task& task) {
    std::vector<std::exception_ptr> errors(count);
    const auto guarded = [&](std::size_t i) {
        try {
            task(i);
        } catch (...) {
            errors[i] = std::current_exception();
        }
    };
    std::vector<std::thread> workers;
    for (std::size_t i = 1; parallel && i < count; ++i) {
        try {
            workers.emplace_back(guarded, i);
        } catch (const std::system_error&) {
            guarded(i);  // No thread to be had: run it here
        }
    }
    guarded(0);
    if (!parallel) {
        for (std::size_t i = 1; i < count; ++i) guarded(i);
    }
    for (std::thread& worker : workers) worker.join();
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

// Everything about one decision that does not depend on the opponents' type.
struct Decision {
    const PlayerView* view = nullptr;
    PotMath math;
    double equity = 0.0;  // Against random hands: the size rule's input
    bool preflop = false;
    bool may_raise = false;
    BoardTexture texture;
    HandRead read;
    std::vector<PotLayer> layers;
    PreflopResponse chart_choice = PreflopResponse::Fold;
    RaiseInputs inputs;  // Without equities, params and pot_kept_share, which each line sets
};

// One line's advice: Baseline's, or one archetype's.
struct LineResult {
    EvEstimates ev;
    EvEstimates base_ev;
    std::array<std::optional<int>, kLadder.size()> tos;
    std::array<std::optional<BetOutcome>, kOutcomeCount> outcomes;
    Action action{};
    std::string label;
    std::string size_reason;
    double range_equity = 0.0;
    bool value_shifted = false;  // The size rule's value bet moved by value_shift
};

// The model, the chart prior and the size rule for one set of opponent ranges and
// params. value_shift moves a postflop value bet that many ladder rungs (Baseline
// 0); prior_scale multiplies the preflop chart prior (Baseline 1).
LineResult evaluate_line(const Decision& d, RangeEquities& equities, const ArchetypeParams& params, int value_shift,
                         double prior_scale) {
    const PlayerView& view = *d.view;
    const LegalActions& legal = view.legal;
    const PotMath& math = d.math;
    const bool preflop = d.preflop;
    const bool may_raise = d.may_raise;

    std::uint8_t everyone = 0;
    for (int i = 0; i < kMaxSeats; ++i) {
        if (i != view.seat && contesting(view.seats[at(i)]) && equities.range(i)) {
            everyone = static_cast<std::uint8_t>(everyone | (1u << i));
        }
    }
    std::vector<std::uint8_t> sets{everyone};
    for (const PotLayer& layer : d.layers) {
        sets.push_back(layer.opponent_seats);
        sets.push_back(all_in_set(view, layer.opponent_seats));
    }
    equities.prepare(sets);

    double layered = 0.0;
    double kept = 0.0;
    int layer_chips = 0;
    for (const PotLayer& layer : d.layers) {
        layered += layer.amount * equities.equity(layer.opponent_seats);
        kept += layer.amount * equities.equity(all_in_set(view, layer.opponent_seats));
        layer_chips += layer.amount;
    }
    const double layered_equity = layer_chips > 0 ? layered / layer_chips : 0.0;

    LineResult r;
    r.range_equity = equities.equity(everyone);

    RaiseInputs inputs = d.inputs;
    if (may_raise) {
        inputs.equities = &equities;
        inputs.params = &params;
        inputs.pot_kept_share = layer_chips > 0 ? kept / layer_chips : 1.0;
    }

    // The chart prior. A free check costs nothing, so it is left alone. Rejected
    // options lose more the more they risk: the chart says most, and the all-in
    // equity of the model least, about the big ones. An approved call gains the
    // same, but an approved raise only the flat amount, or every chart raise would
    // become all in.
    const auto prior = [&](PreflopResponse option, int risked) {
        if (!preflop) return 0.0;
        const double flat = kChartPriorBigBlinds * view.big_blind * prior_scale;
        const double scaled = kChartPriorCallShare * risked * prior_scale;
        if (d.chart_choice != option) return -(flat + scaled);
        return option == PreflopResponse::Call ? flat + scaled : flat;
    };
    const auto raise_risk = [&](int to) { return clamp_raise_to(view, to) - view.seats[at(view.seat)].street_bet; };

    // Later options must beat earlier ones outright, so ties settle on the cheaper play.
    double best_ev = 0.0;
    bool have_best = false;
    const auto consider = [&](double ev, Action action, std::string label) {
        if (have_best && ev <= best_ev) return;
        best_ev = ev;
        r.action = action;
        r.label = std::move(label);
        have_best = true;
    };

    if (legal.allows(ActionType::Check)) {
        const double ev = layered_equity * math.contestable;
        r.ev.check_call = ev;
        consider(ev, {ActionType::Check, 0}, "CHECK");
        r.ev.fold = 0.0;  // Listed for completeness; a free card always beats it
    } else {
        r.ev.fold = 0.0;
        consider(0.0, {ActionType::Fold, 0}, "FOLD");
        if (legal.allows(ActionType::Call)) {
            const double ev = layered_equity * (math.contestable + math.to_call) - math.to_call +
                              prior(PreflopResponse::Call, math.to_call);
            r.ev.check_call = ev;
            consider(ev, {ActionType::Call, 0}, "CALL " + std::to_string(math.to_call));
        }
    }

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
            const bool repeated = std::any_of(r.tos.begin(), r.tos.begin() + static_cast<std::ptrdiff_t>(i),
                                              [&](const std::optional<int>& earlier) { return earlier == to; });
            if (repeated) continue;  // Sizes clamped to the same bet
            const RaiseValue value = raise_value(view, inputs, to);
            const double ev = value.ev + prior(PreflopResponse::Raise, raise_risk(to));
            r.ev.*rung.ev = ev;
            r.tos[i] = to;
            r.outcomes[i] = value.outcome;
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
            r.ev.all_in = value.ev + prior(PreflopResponse::Raise, raise_risk(legal.max_raise_to));
            r.outcomes[kOutcomeAllIn] = value.outcome;
        } else {
            r.ev.all_in = r.ev.check_call.value_or(0.0);
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
        consider(*r.ev.all_in, {ActionType::AllIn, 0}, all_in_label);
    }
    r.base_ev = r.ev;
    // A deep shove is not on offer, so it must not out-list the advice.
    if (preflop && legal.allows(ActionType::AllIn) && !preflop_shove_ok) r.ev.all_in = std::min(*r.ev.all_in, best_ev);

    if (!preflop && may_raise) {
        const bool can_shove = legal.allows(ActionType::AllIn);
        const bool shove_in_range =
            can_shove && (!best_rung_ev || raise_risk(legal.max_raise_to) <= kShoveMaxPots * view.total_pot);
        double best_bet = best_rung_ev.value_or(-std::numeric_limits<double>::infinity());
        if (shove_in_range) best_bet = std::max(best_bet, *r.ev.all_in);

        if (best_bet > best_ev) {  // Betting beats the passive options, on the model alone
            const SizeChoice size = preferred_size(d.texture, d.read, view.board, view.street, d.equity,
                                                   std::max(1, view.opponents_in_hand()));
            double fraction = size.fraction;
            // A value bet moves along the ladder for the type, never past its ends,
            // and onto all-in only where all-in may be chosen anyway.
            if (value_shift != 0 && size.value) {
                const int rung = static_cast<int>(std::lround(fraction * 4.0)) - 1;
                const int shifted = std::clamp(rung + value_shift, 0, static_cast<int>(kLadder.size()) - 1);
                const double shifted_fraction = kLadder[at(shifted)].fraction;
                const int shifted_to = clamp_raise_to(view, raise_to_for_pot_fraction(view, shifted_fraction));
                const bool listed = std::find(r.tos.begin(), r.tos.end(), std::optional<int>(shifted_to)) != r.tos.end();
                if (shifted != rung && (listed || shove_in_range)) {
                    fraction = shifted_fraction;
                    r.value_shifted = true;
                }
            }
            const int target = clamp_raise_to(view, raise_to_for_pot_fraction(view, fraction));
            const auto chosen = std::find(r.tos.begin(), r.tos.end(), std::optional<int>(target));
            const bool shoves_on_ev = shove_in_range && (!best_rung_ev || *r.ev.all_in > *best_rung_ev);
            if (shoves_on_ev || chosen == r.tos.end()) {
                // The chosen size would take the whole stack, or all-in is simply best.
                if (can_shove) {
                    r.ev.all_in = best_bet;
                    consider(best_bet, {ActionType::AllIn, 0}, all_in_label);
                }
            } else {
                const std::size_t index = static_cast<std::size_t>(chosen - r.tos.begin());
                r.ev.*kLadder[index].ev = best_bet;
                consider(best_bet, {raise_type, target}, rung_labels[index]);
                r.size_reason = size.reason;
            }
        }
        // A deep shove is not on offer, so it must not out-list the advice.
        if (can_shove && !shove_in_range) r.ev.all_in = std::min(*r.ev.all_in, best_ev);
    }
    return r;
}

// The outcomes row of a bet or raise this line advises, if it has one.
std::optional<std::size_t> outcome_index(const LineResult& r, const Action& action) {
    if (action.type == ActionType::AllIn) {
        return r.outcomes[kOutcomeAllIn] ? std::optional<std::size_t>(kOutcomeAllIn) : std::nullopt;
    }
    if (action.type != ActionType::Bet && action.type != ActionType::Raise) return std::nullopt;
    for (std::size_t i = 0; i < r.tos.size(); ++i) {
        if (r.tos[i] == action.amount && r.outcomes[i]) return i;
    }
    return std::nullopt;
}

bool aggressive(const Action& action) {
    return action.type == ActionType::Bet || action.type == ActionType::Raise || action.type == ActionType::AllIn;
}

int pct(double value) { return static_cast<int>(std::lround(value)); }

// "BET 150 (75% Pot)" as "BET 150": the why text is short.
std::string short_label(const std::string& label) { return label.substr(0, label.find(" (")); }

// One sentence from the numbers: what drives this line's advice away from
// Baseline's, or why it stays.
std::string line_why(Archetype versus, const LineResult& base, const LineResult& line, const PotMath& math) {
    char text[256];
    const std::string name(to_string(versus));
    const std::string mine_label = short_label(line.label);
    const std::string base_label = short_label(base.label);
    const bool same = line.action.type == base.action.type && line.action.amount == base.action.amount;
    const std::optional<std::size_t> mine = outcome_index(line, line.action);
    const std::optional<std::size_t> theirs = outcome_index(base, base.action);
    const int line_eq = pct(100.0 * line.range_equity);
    const int base_eq = pct(100.0 * base.range_equity);
    const auto fold_at = [](const LineResult& r, std::size_t i) { return pct(r.outcomes[i]->fold_pct); };

    if (same && mine && theirs) {
        std::snprintf(text, sizeof text, "Same as Baseline: folds %d%% to %s (Baseline %d%%); %d%% equity when called.",
                      fold_at(line, *mine), mine_label.c_str(), fold_at(base, *theirs),
                      pct(line.outcomes[*mine]->equity_called_pct));
    } else if (same && math.to_call > 0) {
        std::snprintf(text, sizeof text, "Same as Baseline: %d%% vs the range (Baseline %d%%) against %d%% pot odds.",
                      line_eq, base_eq, pct(100.0 * math.pot_odds));
    } else if (same) {
        std::snprintf(text, sizeof text, "Same as Baseline: %d%% vs the range (Baseline %d%%), and no bet beats checking.",
                      line_eq, base_eq);
    } else if (aggressive(line.action) && aggressive(base.action) && mine && theirs && base.outcomes[*mine] &&
               line.outcomes[*theirs]) {
        // Both bet or raise, at different sizes.
        const bool bigger = *mine > *theirs;
        const std::size_t at_size = bigger ? *mine : *theirs;  // The bigger of the two sizes
        const char* why = !line.value_shifted ? "the better size" : bigger ? "value bets go bigger" : "value bets go smaller";
        std::snprintf(text, sizeof text, "vs %s: %s, not %s: folds %d%% to %s (Baseline %d%%), so %s.", name.c_str(),
                      mine_label.c_str(), base_label.c_str(), fold_at(line, at_size),
                      short_label(bigger ? line.label : base.label).c_str(), fold_at(base, at_size), why);
    } else if (aggressive(line.action) && mine && base.outcomes[*mine]) {
        // Betting or raising where Baseline does not.
        const BetOutcome& o = *line.outcomes[*mine];
        const BetOutcome& b = *base.outcomes[*mine];
        if (o.fold_pct >= b.fold_pct + 5.0) {
            std::snprintf(text, sizeof text, "vs %s: folds %d%% to %s (Baseline %d%%), so betting profits; %d%% equity when called.",
                          name.c_str(), pct(o.fold_pct), mine_label.c_str(), pct(b.fold_pct), pct(o.equity_called_pct));
        } else {
            std::snprintf(text, sizeof text, "vs %s: %d%% equity when called at %s (Baseline %d%%), so it bets thinner for value.",
                          name.c_str(), pct(o.equity_called_pct), mine_label.c_str(), pct(b.equity_called_pct));
        }
    } else if (aggressive(base.action) && theirs && line.outcomes[*theirs]) {
        // Baseline bets or raises and this line does not: what changed at that size.
        const BetOutcome& o = *line.outcomes[*theirs];
        const BetOutcome& b = *base.outcomes[*theirs];
        if (o.fold_pct <= b.fold_pct - 5.0) {
            std::snprintf(text, sizeof text, "vs %s: continues %d%% vs %s (Baseline %d%%): too little fold equity, so %s.",
                          name.c_str(), pct(100.0 - o.fold_pct), base_label.c_str(), pct(100.0 - b.fold_pct),
                          mine_label.c_str());
        } else if (o.raise_pct >= b.raise_pct + 3.0) {
            std::snprintf(text, sizeof text, "vs %s: raises %d%% vs %s (Baseline %d%%), so %s.", name.c_str(),
                          pct(o.raise_pct), base_label.c_str(), pct(b.raise_pct), mine_label.c_str());
        } else {
            std::snprintf(text, sizeof text, "vs %s: only %d%% equity when called at %s (Baseline %d%%), so %s.",
                          name.c_str(), pct(o.equity_called_pct), base_label.c_str(), pct(b.equity_called_pct),
                          mine_label.c_str());
        }
    } else if (math.to_call > 0) {
        std::snprintf(text, sizeof text, "vs %s: %d%% vs the range (Baseline %d%%) against %d%% pot odds, so %s.",
                      name.c_str(), line_eq, base_eq, pct(100.0 * math.pot_odds), mine_label.c_str());
    } else {
        std::snprintf(text, sizeof text, "vs %s: %d%% vs the range (Baseline %d%%), so %s.", name.c_str(), line_eq,
                      base_eq, mine_label.c_str());
    }
    return text;
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

TrainingAdvisor::TrainingAdvisor(std::uint64_t seed, AdvisorOptions options, const ArchetypeParams& baseline_params)
    : TrainingAdvisor(seed, options) {
    baseline_params_ = baseline_params;
}

HintRecommendation TrainingAdvisor::analyze(const PlayerView& view) const {
    if (!view.own_hole || view.legal.mask == 0) {
        throw std::invalid_argument("TrainingAdvisor::analyze: seat is not the one to act");
    }

    const std::array<Card, 2>& cards = *view.own_hole;
    const LegalActions& legal = view.legal;
    const int opponents = std::clamp(view.opponents_in_hand(), 1, kMaxOpponents);
    const std::uint64_t seed = decision_seed(seed_, view);

    Decision d;
    d.view = &view;
    d.math = pot_math(view);
    d.preflop = view.street == Street::Preflop;
    d.may_raise = legal.allows(ActionType::Bet) || legal.allows(ActionType::Raise);
    d.texture = classify_board(view.board);
    d.read = read_hand(cards, view.board);
    d.layers = pot_layers(view);
    const PotMath& math = d.math;
    const bool preflop = d.preflop;

    // Random-hand equity: the spec's raw equity, the log's `eq` and the size rule's input.
    EquityOptions options;
    options.rollouts = options_.rollouts;
    options.seed = seed;
    options.threads = options_.threads;
    EquityTable random_equities(cards, view.board, options);
    d.equity = random_equities.against(opponents);

    RangeEquityOptions range_options;
    range_options.seed = mix(seed ^ kRangeSeedSalt);
    range_options.flop_rivers_per_turn = options_.flop_rivers_per_turn;
    range_options.threads = options_.threads;
    range_options.multiway_rollouts = options_.range_rollouts;

    // Baseline first, then Nit, Station and Maniac when the lines are wanted. The
    // lines run on threads of their own unless one thread is asked for.
    constexpr std::array<Archetype, kArchetypeCount> kLines{Archetype::Baseline, Archetype::Nit, Archetype::Station,
                                                            Archetype::Maniac};
    const std::size_t line_count = options_.archetype_lines ? kLines.size() : 1;
    const bool parallel = options_.threads != 1 && line_count > 1;

    if (d.may_raise) {
        d.inputs.pre = preflop_levels(view);
        d.inputs.standard_to = standard_raise_to(view, d.inputs.pre);
        for (int i = 0; i < kMaxSeats; ++i) {
            if (view.seats[at(i)].position) ++d.inputs.players_dealt;
        }
        if (preflop && d.inputs.pre.raises == 0) {
            for (const ActionRecord& a : view.actions) {
                if (a.type == ActionType::Call) d.inputs.limpers = static_cast<std::uint8_t>(d.inputs.limpers | (1u << a.seat));
            }
        }
    }
    const bool wants_hero_range = d.may_raise && !preflop;

    // The range walks: one per line, and Hero's own range as Baseline opponents
    // would read it, which every line uses as the bettor's range (the types differ
    // in how they answer a bet, not in how they read Hero).
    // baseline_params_, when set, replaces Baseline's row (a tuning hook).
    const ArchetypeParams& baseline_params =
        baseline_params_ ? *baseline_params_ : archetype_params(Archetype::Baseline);
    const auto line_params = [&](std::size_t i) -> const ArchetypeParams& {
        return i == 0 ? baseline_params : archetype_params(kLines[i]);
    };
    std::array<std::array<std::optional<WeightedRange>, kMaxSeats>, kArchetypeCount> ranges;
    WeightedRange hero_range;
    run_tasks(line_count + (wants_hero_range ? 1 : 0), parallel, [&](std::size_t i) {
        if (i < line_count) {
            ranges[i] = infer_ranges(view, line_params(i));
            return;
        }
        hero_range = infer_range_for(view, view.seat, baseline_params, false);
        if (!(hero_range.total() > 0.0)) {
            hero_range = WeightedRange::uniform();
            hero_range.remove_cards(card_mask(std::span<const Card>(view.board)));
        }
    });
    // One per-combo vector covers every line's ranges.
    std::bitset<kComboCount> needed;
    for (std::size_t i = 0; i < line_count; ++i) needed |= prepare_ranges(view, ranges[i]);
    const ComboEquities combos = hero_vs_each_combo(cards, view.board, needed, range_options);

    std::optional<StreetStrength> strength;
    if (wants_hero_range) {
        strength = street_strength(view.board);
        d.inputs.strength = &*strength;
        d.inputs.hero_range = &hero_range;
    }

    // Where the raise a chart wants is not legal, the call is that same choice.
    d.chart_choice = preflop ? chart_response(view) : PreflopResponse::Fold;
    if (d.chart_choice == PreflopResponse::Raise && !d.may_raise) d.chart_choice = PreflopResponse::Call;

    std::array<LineResult, kArchetypeCount> lines;
    run_tasks(line_count, parallel, [&](std::size_t i) {
        const ArchetypeParams& params = line_params(i);
        RangeEquityOptions line_options = range_options;
        double acceptance = 1.0;
        if (i > 0) {
            const int divisor = preflop ? 1 : kLineRolloutDivisor;  // Preflop it is cheap
            const int rollouts = std::max(kMinRangeRollouts, range_options.multiway_rollouts / divisor);
            acceptance = deal_acceptance(ranges[i], mix(range_options.seed ^ i));
            line_options.multiway_rollouts = std::clamp(static_cast<int>(std::min<double>(rollouts, kLineDealBudget * acceptance)),
                                                        kMinRangeRollouts, rollouts);
        }
        RangeEquities equities(view, std::move(ranges[i]), combos, line_options);
        if (!(acceptance > 0.0)) equities.treat_as_independent();
        const double prior_scale = i == 0 ? 1.0 : kArchetypePriorScale;
        lines[i] = evaluate_line(d, equities, params, i == 0 ? 0 : params.value_size_shift, prior_scale);
    });
    const LineResult& base = lines[0];

    HintRecommendation hint;
    hint.equity_pct = d.equity * 100.0;
    hint.range_equity_pct = base.range_equity * 100.0;
    hint.pot_odds_pct = math.pot_odds * 100.0;
    hint.mdf_pct = math.mdf * 100.0;
    hint.spr = math.spr;
    hint.ev = base.ev;
    hint.base_ev = base.base_ev;
    for (std::size_t i = 0; i < kLadder.size(); ++i) hint.*kLadder[i].to = base.tos[i];
    hint.outcomes = base.outcomes;
    hint.best_action = base.label;
    hint.action = base.action;

    for (std::size_t i = 1; i < line_count; ++i) {
        const LineResult& r = lines[i];
        AdviceLine line;
        line.versus = kLines[i];
        line.best_action = r.label;
        line.action = r.action;
        line.ev = r.ev;
        line.range_equity_pct = r.range_equity * 100.0;
        line.outcomes = r.outcomes;
        line.why = line_why(kLines[i], base, r, math);
        hint.versus[i - 1] = std::move(line);
    }

    const std::string_view position = view.position ? to_string(*view.position) : std::string_view("Unknown");
    const std::string_view board_name = texture_name(d.texture, !view.board.empty());

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
    std::string verdict = base.label + " has the highest estimated EV.";
    if (!base.size_reason.empty()) {
        verdict = "Betting beats the passive options, and " + base.label + " fits: " + base.size_reason + ".";
    } else if (base.action.type == ActionType::Fold && hint.equity_pct > hint.pot_odds_pct) {
        verdict = preflop ? "FOLD: the positional chart rejects this hand here, as raw equity overstates it."
                          : "FOLD: equity against random hands overstates this hand against the ranges that bet "
                            "and call here.";
    }
    hint.rationale = std::string(position) + " with " + hand_summary(view, d.read) + " on a " +
                     std::string(board_name == "no board" ? "preflop table with no board" : board_name) +
                     (board_name == "no board" ? "" : " board") + ": " + numbers + ". " + verdict;
    return hint;
}

}  // namespace poker
