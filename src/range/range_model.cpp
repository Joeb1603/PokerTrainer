#include "range/range_model.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "core/hand_evaluator.h"
#include "strategy/preflop_charts.h"

namespace poker {
namespace {

// --- Model constants (see range_model.h) ---------------------------------------

constexpr double kSmallBetMax = 0.4;  // Own-range rule: bets up to this many pots widen value...
constexpr double kSmallBetValueMult = 1.6;
constexpr double kMediumBetMax = 0.8;
constexpr double kLargeBetValueMult = 0.75;  // ... and bigger ones narrow it
constexpr double kAirTopShare = 0.6;         // Weaker than this, with no strong draw: air
constexpr double kMediumBetBase = 0.15;      // Medium hands bet this at value_bet_top 0.33
constexpr double kReferenceValueTop = 0.33;
constexpr double kCheckProbeSize = 0.5;      // A check is "did not bet half pot"
constexpr double kContinueCap = 0.95;
// Value against the opponents: the continue share assumed for whoever calls is a
// standard player's, reference_continue(s), whatever the actor's own type.
constexpr double kReferenceContinueMult = 0.95;
constexpr double kReferenceContinueFloor = 0.10;
constexpr double kAggressionShift = 1.0;     // Thresholds move by this x (value_bet_top - 0.33)
constexpr double kRiverValueMargin = 0.6;    // River value: ahead of 70% of the calls, not half
constexpr double kRaiseContinueShare = 0.5;  // Facing a raise, a bettor continues half the reference share
constexpr double kBarrelBluffMult = 4.0;     // The aggressor's flop and turn bluff ratio, x the river's
constexpr double kMaxBluffWeight = 0.9;      // Some air always gives up
constexpr double kFlopDrawEquity = 0.35;
constexpr double kTurnDrawEquity = 0.18;
constexpr double kDrawPriceSlack = 0.9;      // Draws call when equity >= 0.9 x pot odds
constexpr double kDrawContinueOtherwise = 0.3;
constexpr double kRaiseDrawShare = 0.5;      // Strong draws raise at draw_bet x this
constexpr double kRaiseAirBase = 0.1;        // Own-range rule: air raises at bluff_mult x this
// The raise share cap scales by clamp(this / (1 + s), min, max) for a bet of s pots:
// 1 at half pot, less against big bets, more against small ones.
constexpr double kRaiseScaleNumerator = 1.5;
constexpr double kRaiseScaleMin = 0.4;
constexpr double kRaiseScaleMax = 1.25;
// Fallback: a product is kept if it holds this many combos or this share of the
// range before the action.
constexpr double kMinRangeCombos = 0.01 * kComboCount;
constexpr double kMinActionChance = 0.005;
constexpr double kEmptyMass = 1e-6;

// Preflop.
constexpr double kIsoWidth = 0.75;
constexpr double kLimpCallShare = 0.6;   // A limp-call keeps the strongest 0.6 x flat_width of the window
constexpr double kLimpRaiseShare = 0.1;  // A limp-raise: traps plus the strongest 0.1 x three_bet_width
constexpr int kShortAllInBigBlinds = 15;
constexpr double kPushPercentTimesBigBlinds = 400.0;  // Temporary push/fold stand-ins
constexpr double kPushMinPercent = 5.0;
constexpr double kReshoveShare = 0.5;
constexpr double kCallPushPercentTimesBigBlinds = 250.0;
constexpr double kCallPushMinPercent = 8.0;
constexpr double kCommitStackShare = 0.40;
constexpr int kCommitPriceBigBlinds = 10;
constexpr int kCommitAlwaysBigBlinds = 50;

// A smooth step from 1 (t at least 2 widths below the threshold) to 0 (2 widths
// above), 0.5 at the threshold: a cubic, so nothing leaks past the edge. With
// many weak hands and a narrow value region, a logistic's tails outweighed the
// value region itself.
double edge(double threshold, double t, const ArchetypeParams& p) {
    const double width = p.softness > 1e-9 ? p.softness : 1e-9;
    const double x = std::clamp((threshold - t) / (4.0 * width) + 0.5, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

// Sizes and prices come from chip counts, so a bad one only means a bad record.
double safe_size(double size) { return std::isfinite(size) && size > 0.0 ? size : 0.0; }

double value_top(double size, const ArchetypeParams& p) {
    const double mult = size <= kSmallBetMax ? kSmallBetValueMult : size <= kMediumBetMax ? 1.0 : kLargeBetValueMult;
    return p.value_bet_top * mult;
}

// The share of a standard player's range that continues facing a bet of `size` pots.
double reference_continue(double size) {
    return std::clamp(kReferenceContinueMult / (1.0 + size), kReferenceContinueFloor, kContinueCap);
}

// A raise to three times a bet of `faced` pots, in pots after calling: 2s / (1 + 2s).
double assumed_raise_size(double faced) { return 2.0 * faced / (1.0 + 2.0 * faced); }

double aggression_shift(const ArchetypeParams& p) {
    return kAggressionShift * (p.value_bet_top - kReferenceValueTop);
}

double draw_equity(Street street) {
    if (street == Street::Flop) return kFlopDrawEquity;
    return street == Street::Turn ? kTurnDrawEquity : 0.0;
}

double live_total(const WeightedRange& range, const StreetStrength& s) {
    double total = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (s.live.test(static_cast<std::size_t>(combo))) total += range.weight(combo);
    }
    return total;
}

using Shares = std::array<float, kComboCount>;

// Where each combo stands, for bets and raises. `own` is its top_share in the
// actor's own range. `vs` is its standing against the opponents, the largest
// top_share it holds in any of their ranges; under the own-range rule (no opponent
// range known) it is `own` again and `relative` is set.
struct Footing {
    const Shares& own;
    const Shares& vs;
    bool relative = false;
    bool aggressor = false;  // The actor made the last bet or raise of the previous street
};

// The value threshold on `vs` for a bet of `size` pots on a board of `board_size` cards.
double bet_value_threshold(double size, int board_size, bool relative, const ArchetypeParams& p) {
    if (relative) return value_top(size, p);
    const double margin = board_size >= 5 ? kRiverValueMargin : 1.0;
    return 0.5 * margin * reference_continue(size) + aggression_shift(p);
}

// The value threshold on `vs` for a raise of a bet of `faced` pots.
double raise_value_threshold(double faced, bool relative, const ArchetypeParams& p) {
    if (relative) return p.value_bet_top / 2.0;
    return 0.5 * kRaiseContinueShare * reference_continue(assumed_raise_size(faced)) + aggression_shift(p);
}

BetBreakdown bet_breakdown_at(const WeightedRange& range, const StreetStrength& s, const Footing& f, double size,
                              const ArchetypeParams& p) {
    BetBreakdown out;
    const double v = bet_value_threshold(size, s.board_size, f.relative, p);
    const double r = size / (1.0 + size);
    const double medium = clamp01(kMediumBetBase * p.value_bet_top / kReferenceValueTop);
    // The previous street's aggressor, with cards to come, bluffs more than the
    // river ratio: its air has equity and its story carries fold equity.
    const bool barrel = !f.relative && f.aggressor && s.board_size < 5;
    const double air_mult = barrel ? kBarrelBluffMult : 1.0;
    const double draw = clamp01(p.draw_bet);
    // A bet under half pot keeps the opponent's range in, so value slowplays less.
    const double slow = f.relative ? 1.0 : std::min(1.0, size / kCheckProbeSize);
    const double hold_back = 1.0 - clamp01(p.slowplay) * slow;
    const auto air = [&](std::size_t i) { return s.draw[i] != kStrongDraw && f.vs[i] > kAirTopShare; };

    std::array<double, kComboCount> value{};
    double air_room = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        value[i] = edge(v, f.vs[i], p) * hold_back;
        const double w = range.weight(combo);
        out.range_mass += w;
        out.value_mass += w * value[i];
        if (air(i)) air_room += w * (1.0 - value[i]);
    }
    const double target = air_mult * std::max(0.0, p.bluff_mult) * r * out.value_mass;
    out.bluff_weight = air_room > 0.0 ? std::min(f.relative ? 1.0 : kMaxBluffWeight, target / air_room) : 0.0;

    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        double x = 0.0;
        double* mass = nullptr;
        if (s.draw[i] == kStrongDraw) {
            x = draw;
            mass = &out.draw_mass;
        } else if (air(i)) {
            x = out.bluff_weight;
            mass = &out.air_mass;
        } else {
            x = medium;
            mass = &out.medium_mass;
        }
        const double rest = (1.0 - value[i]) * x;
        *mass += range.weight(combo) * rest;
        out.likelihood.set(combo, static_cast<float>(value[i] + rest));
    }
    return out;
}

}  // namespace

double raise_share_scale(double size_pots) {
    return std::clamp(kRaiseScaleNumerator / (1.0 + safe_size(size_pots)), kRaiseScaleMin, kRaiseScaleMax);
}

namespace {

// P(raise | combo) facing a bet of `size` pots, already scaled to the size's
// raise share cap.
std::array<double, kComboCount> raise_probabilities(const WeightedRange& range, const StreetStrength& s,
                                                    const Footing& f, double size, const ArchetypeParams& p) {
    std::array<double, kComboCount> value{};
    const double v = raise_value_threshold(size, f.relative, p);
    const double hold_back = 1.0 - clamp01(p.slowplay);
    const double draw = clamp01(p.draw_bet * kRaiseDrawShare);
    // Bluff raises come from the bottom of the raiser's own range.
    const auto air = [&](std::size_t i) { return s.draw[i] != kStrongDraw && f.own[i] > kAirTopShare; };
    double mass = 0.0;
    double value_mass = 0.0;
    double air_room = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        value[i] = edge(v, f.vs[i], p) * hold_back;
        const double w = range.weight(combo);
        mass += w;
        value_mass += w * value[i];
        if (air(i)) air_room += w * (1.0 - value[i]);
    }
    double bluff = clamp01(kRaiseAirBase * std::max(0.0, p.bluff_mult));
    if (!f.relative) {
        const double raise = assumed_raise_size(size);
        const double target = std::max(0.0, p.bluff_mult) * raise / (1.0 + raise) * value_mass;
        bluff = air_room > 0.0 ? std::min(1.0, target / air_room) : 0.0;
    }

    std::array<double, kComboCount> q{};
    double raising = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        const double y = s.draw[i] == kStrongDraw ? draw : air(i) ? bluff : 0.0;
        q[i] = value[i] + (1.0 - value[i]) * y;
        raising += range.weight(combo) * q[i];
    }
    const double share = std::max(0.0, p.raise_share) * raise_share_scale(size);
    const double k = mass > 0.0 && raising > share * mass ? share * mass / raising : 1.0;
    for (double& x : q) x *= k;
    return q;
}

// P(continue | combo) facing a bet, before the raise split, for a continue
// threshold `target` on top_share.
std::array<double, kComboCount> continue_probabilities(const StreetStrength& s, const Shares& t, Street street,
                                                       double target, double pot_odds, const ArchetypeParams& p) {
    std::array<double, kComboCount> c{};
    const double draw = draw_equity(street) >= kDrawPriceSlack * pot_odds ? 1.0 : kDrawContinueOtherwise;
    const bool has_draws = street == Street::Flop || street == Street::Turn;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        double value = edge(target, t[i], p);
        if (has_draws && s.draw[i] == kStrongDraw) value = std::max(value, draw);
        if (p.pairs_and_draws_continue && (s.holds_pair.test(i) || s.draw[i] != kNoDraw)) {
            value = std::max(value, clamp01(p.pairs_and_draws_prob));
        }
        c[i] = value;
    }
    return c;
}

struct Answer {
    std::array<double, kComboCount> raise{};
    std::array<double, kComboCount> cont{};  // Call or raise, before the raise split
};

// Facing a bet. Bluff raises come out of the continue budget: the threshold is
// lowered by the share of the range that would fold but raises instead, so the
// range still continues about C in all (one correction step, not iterated).
Answer answer_probabilities(const WeightedRange& range, const StreetStrength& s, const Footing& f, Street street,
                            double size, double pot_odds, const ArchetypeParams& p) {
    Answer out;
    out.raise = raise_probabilities(range, s, f, size, p);
    const double mdf = 1.0 / (1.0 + size);
    const double target = std::min(std::max(p.continue_mult * mdf, p.continue_floor), kContinueCap);
    const auto first = continue_probabilities(s, f.own, street, target, pot_odds, p);
    double mass = 0.0;
    double folders_raising = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        mass += range.weight(combo);
        folders_raising += range.weight(combo) * (1.0 - first[i]) * out.raise[i];
    }
    const double shift = mass > 0.0 ? folders_raising / mass : 0.0;
    out.cont = continue_probabilities(s, f.own, street, std::max(0.0, target - shift), pot_odds, p);
    return out;
}

Response postflop_response_at(const WeightedRange& range, const StreetStrength& s, const Footing& f, Street street,
                              double size, double pot_odds, const ArchetypeParams& p) {
    const Answer answer = answer_probabilities(range, s, f, street, size, pot_odds, p);
    Response out;
    double mass = 0.0;
    double calls = 0.0;
    double raises = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (!s.live.test(i)) continue;
        const double call = answer.cont[i] * (1.0 - answer.raise[i]);
        const double w = range.weight(combo);
        mass += w;
        calls += w * call;
        raises += w * answer.raise[i];
        out.call_range.set(combo, static_cast<float>(w * call));
        out.raise_range.set(combo, static_cast<float>(w * answer.raise[i]));
    }
    if (mass > 0.0) {
        out.call = calls / mass;
        out.raise = raises / mass;
        out.fold = std::max(0.0, 1.0 - out.call - out.raise);
    }
    return out;
}

WeightedRange call_likelihood_at(const WeightedRange& range, const StreetStrength& s, const Footing& f, Street street,
                                 double size, double pot_odds, const ArchetypeParams& p) {
    const Answer answer = answer_probabilities(range, s, f, street, size, pot_odds, p);
    WeightedRange out;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (s.live.test(i)) out.set(combo, static_cast<float>(answer.cont[i] * (1.0 - answer.raise[i])));
    }
    return out;
}

WeightedRange raise_likelihood_at(const WeightedRange& range, const StreetStrength& s, const Footing& f, double size,
                                  const ArchetypeParams& p) {
    const auto raise = raise_probabilities(range, s, f, size, p);
    WeightedRange out;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const auto i = static_cast<std::size_t>(combo);
        if (s.live.test(i)) out.set(combo, static_cast<float>(raise[i]));
    }
    return out;
}

// 1 - scale * P(bet half pot): scale is 1, or lead_share when checking to the aggressor.
WeightedRange check_likelihood_at(const WeightedRange& range, const StreetStrength& s, const Footing& f, double scale,
                                  const ArchetypeParams& p) {
    const BetBreakdown bet = bet_breakdown_at(range, s, f, kCheckProbeSize, p);
    const double k = clamp01(scale);
    WeightedRange out;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (s.live.test(static_cast<std::size_t>(combo))) {
            out.set(combo, static_cast<float>(1.0 - k * bet.likelihood.weight(combo)));
        }
    }
    return out;
}

// Footing for the public functions: against `opponent` when given, else the own-range rule.
struct OwnedFooting {
    Shares own;
    Shares vs;
    bool relative;

    OwnedFooting(const WeightedRange& range, const WeightedRange* opponent, const StreetStrength& s)
        : own(top_share(range, s)), vs(opponent ? top_share(*opponent, s) : own), relative(opponent == nullptr) {}
    Footing footing() const { return Footing{own, vs, relative}; }
};

// --- Preflop charts -------------------------------------------------------------

struct ExtraCharts {
    VsRaiseRanges vs_three_bet;
    VsRaiseRanges vs_four_bet;
    HandRange commit;     // vs-4-bet raise and call together
    HandRange bb_raise;   // Premium and Strong tiers
    HandRange premiums;

    ExtraCharts() {
        for (const HandClass hand : hand_classes_by_strength()) {
            const PreflopResponse three = vs_three_bet_response(hand);
            const PreflopResponse four = vs_four_bet_response(hand);
            if (three == PreflopResponse::Raise) vs_three_bet.raise.add(hand);
            if (three == PreflopResponse::Call) vs_three_bet.call.add(hand);
            if (four == PreflopResponse::Raise) vs_four_bet.raise.add(hand);
            if (four == PreflopResponse::Call) vs_four_bet.call.add(hand);
            if (four != PreflopResponse::Fold) commit.add(hand);
            if (preflop_tier(hand) <= PreflopTier::Strong) bb_raise.add(hand);
            if (preflop_tier(hand) == PreflopTier::Premium) premiums.add(hand);
        }
    }
};

const ExtraCharts& extra_charts() {
    static const ExtraCharts charts;
    return charts;
}

// The cap limits growth only, and never below the chart itself: a width under 1
// is a plain shrink, and a growth stops at max(max_range_pct, the chart's own %).
double growth_cap(double chart_percent, double width, const ArchetypeParams& p) {
    if (!(width > 1.0)) return 100.0;
    return std::max(p.max_range_pct, chart_percent + 1e-9);  // The margin keeps the chart's last class
}

HandRange resized(const HandRange& chart, double width, const ArchetypeParams& p) {
    return resize_range(chart, width, growth_cap(chart.percent(), width, p));
}

// Call and raise probabilities per class for a chart, as in the header table.
struct ChartAnswer {
    WeightedRange call;
    WeightedRange raise;
};

ChartAnswer chart_answer(const VsRaiseRanges& chart, const HandRange& call_chart, double raise_width,
                         double call_width, const ArchetypeParams& p) {
    const HandRange r3 = resized(chart.raise, raise_width, p);
    const HandRange cc = resized(call_chart, call_width, p);
    const auto sp = static_cast<float>(clamp01(p.slowplay));
    ChartAnswer out;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (r3.contains(hand)) {
            out.raise.set_class(hand, 1.0f - sp);
            out.call.set_class(hand, sp);
        } else if (cc.contains(hand) || chart.raise.contains(hand)) {
            out.call.set_class(hand, 1.0f);
        }
    }
    return out;
}

// The chart, call chart and widths for a player facing its `raises`-th raise.
ChartAnswer facing_raise_answer(Position pos, Position opener, int raises, int players, double call_scale,
                                const ArchetypeParams& p) {
    if (raises <= 1) {
        const VsRaiseRanges& chart = vs_open_ranges(pos, opener, players);
        // The SB charts are 3-bet or fold; a SB that flats plays like a caller in position.
        const HandRange& calls = chart.call.empty() ? vs_open_ranges(Position::BTN, opener, players).call : chart.call;
        return chart_answer(chart, calls, p.three_bet_width, p.flat_width * call_scale, p);
    }
    const ExtraCharts& extra = extra_charts();
    const VsRaiseRanges& chart = raises == 2 ? extra.vs_three_bet : extra.vs_four_bet;
    return chart_answer(chart, chart.call, p.four_bet_width, p.call_3bet_width * call_scale, p);
}

HandRange open_range(Position pos, int players, const ArchetypeParams& p) {
    return resized(rfi_range(pos, players), p.open_width, p);
}

// The limp window: the strongest limp_pct % of combos outside the open range.
HandRange limp_window(Position pos, int players, const ArchetypeParams& p) {
    const HandRange open = open_range(pos, players, p);
    const double window_combos = std::max(0.0, p.limp_pct) * kTotalCombos / 100.0;
    HandRange grown;
    if (open.combos() > 0) {
        grown = resize_range(open, (open.combos() + window_combos) / open.combos(), 100.0);
    } else {
        grown = HandRange::top_percent(std::max(0.0, p.limp_pct));
    }
    HandRange out;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (grown.contains(hand) && !open.contains(hand)) out.add(hand);
    }
    return out;
}

// Premiums normally sit inside the open range, so they limp only as traps.
bool limp_trap_class(HandClass hand, const HandRange& window) {
    return extra_charts().premiums.contains(hand) && !window.contains(hand);
}

WeightedRange limp_likelihood(Position pos, int players, const ArchetypeParams& p) {
    const HandRange window = limp_window(pos, players, p);
    const auto trap = static_cast<float>(clamp01(p.limp_trap));
    WeightedRange out;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (window.contains(hand)) out.set_class(hand, 1.0f);
        else if (limp_trap_class(hand, window)) out.set_class(hand, trap);
    }
    return out;
}

// A limper facing the first raise. Raise: the traps and the strongest
// kLimpRaiseShare x three_bet_width of the window, at 1 - sp. Call: sp there, and 1
// in the strongest kLimpCallShare x flat_width of the window (call_scale scales
// that width, as for a chart's calls).
ChartAnswer limper_answer(Position pos, int players, double call_scale, const ArchetypeParams& p) {
    const HandRange window = limp_window(pos, players, p);
    const HandRange raising = resize_range(window, clamp01(kLimpRaiseShare * p.three_bet_width), 100.0);
    const HandRange calling = resize_range(window, clamp01(kLimpCallShare * p.flat_width * call_scale), 100.0);
    const auto sp = static_cast<float>(clamp01(p.slowplay));
    ChartAnswer out;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (raising.contains(hand) || limp_trap_class(hand, window)) {
            out.raise.set_class(hand, 1.0f - sp);
            out.call.set_class(hand, sp);
        } else if (calling.contains(hand)) {
            out.call.set_class(hand, 1.0f);
        }
    }
    return out;
}

// The top `percent` % by strength times `width`, capped as resized() caps growth.
WeightedRange top_percent_range(double percent, double width, const ArchetypeParams& p) {
    const double target = percent * std::max(0.0, width);
    return WeightedRange::from_hand_range(HandRange::top_percent(std::min(target, growth_cap(percent, width, p))));
}

// --- The walk over a view's actions --------------------------------------------

struct Tracked {
    WeightedRange range = WeightedRange::uniform();
    WeightedRange street_start = WeightedRange::uniform();  // As the current street began
    WeightedRange preflop = WeightedRange::uniform();       // As preflop ended
    bool dealt = false;
    bool folded = false;
    bool limped = false;  // Called preflop before any raise (a limp, or the SB completing)
    int start_stack = 0;
    int committed = 0;
    int street_bet = 0;
};

// Every dealt player's range is tracked, whichever ones the caller wants, because
// a bet or raise is judged against the other players' ranges.
class RangeWalk {
public:
    RangeWalk(const PlayerView& view, const ArchetypeParams& params, bool remove_viewer_cards)
        : view_(view), p_(params), tracked_(kMaxSeats), current_bet_(view.big_blind),
          last_full_raise_(view.big_blind) {
        for (int i = 0; i < kMaxSeats; ++i) {
            const PublicSeat& s = view.seats[static_cast<std::size_t>(i)];
            if (s.position) ++players_;
            Tracked& t = tracked_[static_cast<std::size_t>(i)];
            t.dealt = s.position.has_value();
            t.start_stack = s.stack + s.total_bet;
            // Blinds are not action records: whatever a player put in beyond its
            // records is its blind.
            int recorded = 0;
            for (const ActionRecord& a : view.actions) {
                if (a.seat == i) recorded += a.chips_added;
            }
            t.committed = std::max(0, s.total_bet - recorded);
            t.street_bet = t.committed;
            if (!t.dealt) t.range = WeightedRange{};
        }
        if (remove_viewer_cards && view.own_hole) viewer_dead_ = card_mask(std::span<const Card>(*view.own_hole));
        apply_dead();
        for (Tracked& t : tracked_) t.street_start = t.preflop = t.range;
    }

    void run() {
        for (const ActionRecord& a : view_.actions) {
            if (a.seat < 0 || a.seat >= kMaxSeats || a.street > Street::River) continue;
            if (a.street != street_) enter_street(a.street);
            act(a);
        }
        // The board as it stands, including streets nobody has acted on yet.
        board_dead_ |= card_mask(std::span<const Card>(view_.board));
        apply_dead();
    }

    const WeightedRange& range(int i) const { return tracked_[static_cast<std::size_t>(i)].range; }

private:
    // The viewer's own range never loses the viewer's cards.
    CardMask dead_for(int i) const { return board_dead_ | (i == view_.seat ? CardMask{0} : viewer_dead_); }

    void apply_dead() {
        for (int i = 0; i < kMaxSeats; ++i) {
            Tracked& t = tracked_[static_cast<std::size_t>(i)];
            if (t.dealt) t.range.remove_cards(dead_for(i));
        }
    }

    void enter_street(Street street) {
        if (street_ == Street::Preflop) {
            for (Tracked& t : tracked_) t.preflop = t.range;
        }
        previous_aggressor_ = street_aggressor_;
        street_aggressor_ = -1;
        acted_ = 0;
        street_ = street;
        current_bet_ = 0;
        last_full_raise_ = view_.big_blind;
        added_ = 0;
        last_aggression_size_ = 0.0;
        last_aggression_all_in_ = false;
        last_aggression_total_ = 0;
        for (Tracked& t : tracked_) t.street_bet = 0;
        const std::size_t shown = street == Street::Flop ? 3 : street == Street::Turn ? 4 : 5;
        const std::size_t n = std::min(shown, view_.board.size());
        board_dead_ |= card_mask(std::span<const Card>(view_.board.data(), n));
        apply_dead();
        for (Tracked& t : tracked_) t.street_start = t.range;
    }

    const StreetStrength& strength() {
        const auto index = static_cast<std::size_t>(street_) - 1;
        if (!strengths_[index]) {
            const std::size_t shown = street_ == Street::Flop ? 3 : street_ == Street::Turn ? 4 : 5;
            if (view_.board.size() < shown) throw std::invalid_argument("infer_ranges: board missing for a street");
            strengths_[index] = street_strength(std::span<const Card>(view_.board.data(), shown));
        }
        return *strengths_[index];
    }

    // A product too small to trust means the model all but ruled the action out.
    // The action's likelihood is then kept on the range as this street began, or
    // failing that as preflop ended, or alone; if all are empty it is ignored.
    void narrow(Tracked& t, const WeightedRange& likelihood, int i) {
        const auto product_of = [&](const WeightedRange& base) {
            WeightedRange product = base;
            product.multiply(likelihood);
            product.remove_cards(dead_for(i));
            return product;
        };
        const auto trusted = [](const WeightedRange& product, double before) {
            const double after = product.total();
            return after >= kMinRangeCombos || after >= kMinActionChance * before;
        };
        WeightedRange product = product_of(t.range);
        if (trusted(product, t.range.total())) {
            t.range = product;
            return;
        }
        product = product_of(t.street_start);
        if (trusted(product, t.street_start.total())) {
            t.range = product;
            return;
        }
        product = product_of(t.preflop);
        if (product.total() <= kEmptyMass) product = product_of(WeightedRange::uniform());
        if (product.total() > kEmptyMass) t.range = product;
    }

    void act(const ActionRecord& a) {
        Tracked& t = tracked_[static_cast<std::size_t>(a.seat)];
        const int facing = std::max(0, current_bet_ - t.street_bet);
        const int pot_before = view_.street_start_pot[static_cast<std::size_t>(street_)] + added_;
        const bool aggressive = a.type == ActionType::Bet || a.type == ActionType::Raise;

        if (t.dealt && !t.folded && a.type != ActionType::Fold) {
            if (street_ == Street::Preflop) {
                if (const auto likelihood = preflop_likelihood(a, t, facing)) narrow(t, *likelihood, a.seat);
            } else {
                narrow(t, postflop_likelihood(a, t, pot_before), a.seat);
            }
        }

        if (aggressive) {
            const int increment = std::max(0, a.street_total - current_bet_);
            const double pot_after_call = pot_before + facing;
            last_aggression_size_ = pot_after_call > 0 ? increment / pot_after_call : 1.0;
            last_aggression_all_in_ = a.all_in;
            last_aggression_total_ = a.street_total;
            street_aggressor_ = a.seat;
            // As the engine rules: only a full raise (an increment of at least the
            // last full one) is a new level. A short all-in is not, so a later
            // re-raise reads as it would have without it. The first raise counts.
            const bool full = increment >= last_full_raise_;
            if (full) last_full_raise_ = increment;
            current_bet_ = std::max(current_bet_, a.street_total);
            if (street_ == Street::Preflop) {
                if (raises_ == 0) opener_ = position_of(a.seat);
                if (full || raises_ == 0) ++raises_;
            }
        } else if (a.type == ActionType::Call && street_ == Street::Preflop && raises_ == 0) {
            ++limpers_;
            t.limped = true;
        }
        if (a.type == ActionType::Fold) t.folded = true;
        acted_ = static_cast<std::uint8_t>(acted_ | (1u << a.seat));
        t.street_bet += a.chips_added;
        t.committed += a.chips_added;
        added_ += a.chips_added;
    }

    Position position_of(int i) const {
        return view_.seats[static_cast<std::size_t>(i)].position.value_or(Position::BTN);
    }

    double big_blinds(int chips) const {
        return view_.big_blind > 0 ? static_cast<double>(chips) / view_.big_blind : 0.0;
    }

    bool call_is_committing(const ActionRecord& a, const Tracked& t) const {
        const int call = a.chips_added;
        if (call <= view_.big_blind * kCommitPriceBigBlinds) return false;
        if (call >= view_.big_blind * kCommitAlwaysBigBlinds) return true;
        int deepest = 0;
        for (int i = 0; i < kMaxSeats; ++i) {
            const Tracked& o = tracked_[static_cast<std::size_t>(i)];
            if (i == a.seat || o.folded || !o.dealt) continue;
            deepest = std::max(deepest, o.start_stack - (o.committed - o.street_bet));
        }
        const int my_stack = t.start_stack - t.committed;
        return call * 3 >= std::min(my_stack, deepest - t.street_bet);
    }

    std::optional<WeightedRange> preflop_likelihood(const ActionRecord& a, const Tracked& t, int facing) const {
        const Position pos = position_of(a.seat);
        const ExtraCharts& extra = extra_charts();
        const bool aggressive = a.type == ActionType::Bet || a.type == ActionType::Raise;
        const bool limper_facing_raise = t.limped && raises_ == 1;

        if (aggressive) {
            if (a.all_in && big_blinds(a.street_total) <= kShortAllInBigBlinds) {
                const double bb = std::max(1.0, big_blinds(a.street_total));
                double percent = std::clamp(kPushPercentTimesBigBlinds / bb, kPushMinPercent, 100.0);
                if (raises_ > 0) percent *= kReshoveShare;
                return top_percent_range(percent, p_.open_width, p_);
            }
            if (a.all_in || a.street_total >= kCommitStackShare * t.start_stack) {
                return WeightedRange::from_hand_range(resized(extra.commit, p_.four_bet_width, p_));
            }
            if (raises_ == 0) {
                if (limpers_ == 0) return WeightedRange::from_hand_range(open_range(pos, players_, p_));
                if (pos == Position::BB) return WeightedRange::from_hand_range(resized(extra.bb_raise, p_.open_width, p_));
                return WeightedRange::from_hand_range(resized(rfi_range(pos, players_), kIsoWidth * p_.open_width, p_));
            }
            if (limper_facing_raise) return limper_answer(pos, players_, 1.0, p_).raise;
            return facing_raise_answer(pos, opener_, raises_, players_, 1.0, p_).raise;
        }

        if (a.type == ActionType::Check) {
            if (raises_ > 0 || facing > 0) return std::nullopt;
            // The big blind's free option: everything but its raising hands.
            const HandRange raising = resized(extra.bb_raise, p_.open_width, p_);
            WeightedRange out = WeightedRange::uniform();
            for (const HandClass hand : hand_classes_by_strength()) {
                if (raising.contains(hand)) out.set_class(hand, static_cast<float>(clamp01(p_.slowplay)));
            }
            return out;
        }

        // A call.
        if (raises_ == 0) return limp_likelihood(pos, players_, p_);
        if (last_aggression_all_in_ && big_blinds(last_aggression_total_) <= kShortAllInBigBlinds) {
            const double bb = std::max(1.0, big_blinds(last_aggression_total_));
            const double percent = std::clamp(kCallPushPercentTimesBigBlinds / bb, kCallPushMinPercent, 100.0);
            return top_percent_range(percent, p_.flat_width, p_);
        }
        if (call_is_committing(a, t)) {
            return WeightedRange::from_hand_range(resized(extra.commit, p_.call_3bet_width, p_));
        }
        if (limper_facing_raise) return limper_answer(pos, players_, 1.0, p_).call;
        return facing_raise_answer(pos, opener_, raises_, players_, 1.0, p_).call;
    }

    // The standing of every combo against the players `i` plays against: the
    // largest top_share it holds in any of their current ranges. False when there
    // is nobody left, and then the own-range rule applies.
    bool standing(int i, const StreetStrength& s, Shares& out) const {
        out.fill(0.0f);
        bool any = false;
        for (int j = 0; j < kMaxSeats; ++j) {
            const Tracked& o = tracked_[static_cast<std::size_t>(j)];
            if (j == i || !o.dealt || o.folded) continue;
            any = true;
            const Shares share = top_share(o.range, s);
            for (std::size_t c = 0; c < share.size(); ++c) out[c] = std::max(out[c], share[c]);
        }
        return any;
    }

    WeightedRange postflop_likelihood(const ActionRecord& a, const Tracked& t, int pot_before) {
        const StreetStrength& s = strength();
        const Shares own = top_share(t.range, s);
        Shares vs{};
        const bool against = standing(a.seat, s, vs);
        const Footing f{own, against ? vs : own, !against, previous_aggressor_ == a.seat};
        switch (a.type) {
            case ActionType::Check:
                return check_likelihood_at(t.range, s, f, checks_to_aggressor(a.seat) ? p_.lead_share : 1.0, p_);
            case ActionType::Bet:
            case ActionType::Raise:
                if (current_bet_ == 0) {
                    const double size = pot_before > 0 ? static_cast<double>(a.chips_added) / pot_before : 1.0;
                    return bet_breakdown_at(t.range, s, f, safe_size(size), p_).likelihood;
                }
                return raise_likelihood_at(t.range, s, f, safe_size(last_aggression_size_), p_);
            case ActionType::Call: {
                const double odds = static_cast<double>(a.chips_added) / std::max(1, pot_before + a.chips_added);
                return call_likelihood_at(t.range, s, f, street_, safe_size(last_aggression_size_), odds, p_);
            }
            case ActionType::Fold:
            case ActionType::AllIn:
                break;
        }
        return WeightedRange::uniform();
    }

    const PlayerView& view_;
    const ArchetypeParams& p_;
    std::vector<Tracked> tracked_;  // On the heap: three ranges per player
    std::array<std::optional<StreetStrength>, 3> strengths_{};
    CardMask board_dead_ = 0;
    CardMask viewer_dead_ = 0;
    int players_ = 0;

    Street street_ = Street::Preflop;
    int current_bet_ = 0;
    int last_full_raise_ = 0;  // The increment a raise needs to be a new level
    int added_ = 0;
    int raises_ = 0;
    int limpers_ = 0;
    Position opener_ = Position::BTN;
    double last_aggression_size_ = 0.0;
    bool last_aggression_all_in_ = false;
    int last_aggression_total_ = 0;
    int street_aggressor_ = -1;    // Last to bet or raise on this street
    int previous_aggressor_ = -1;  // ... and on the street before
    std::uint8_t acted_ = 0;       // Bit per seat index: acted on this street

    // A check to the player who bet or raised last on the previous street, who is
    // still to act here: the standard play with nearly every hand.
    bool checks_to_aggressor(int i) const {
        if (previous_aggressor_ < 0 || previous_aggressor_ == i) return false;
        const Tracked& aggressor = tracked_[static_cast<std::size_t>(previous_aggressor_)];
        return !aggressor.folded && ((acted_ >> previous_aggressor_) & 1u) == 0;
    }
};

}  // namespace

// --- Archetypes -----------------------------------------------------------------

const ArchetypeParams& archetype_params(Archetype archetype) {
    static const std::array<ArchetypeParams, kArchetypeCount> table = [] {
        std::array<ArchetypeParams, kArchetypeCount> t{};
        t[0] = ArchetypeParams{};  // Baseline

        ArchetypeParams& nit = t[1];
        nit.open_width = 0.60;
        nit.max_range_pct = 15.0;
        nit.limp_pct = 1.0;
        nit.limp_trap = 0.5;
        nit.flat_width = 0.60;
        nit.three_bet_width = 0.5;
        nit.call_3bet_width = 0.7;
        nit.four_bet_width = 0.6;
        nit.continue_mult = 0.70;
        nit.continue_floor = 0.05;
        nit.raise_share = 0.05;
        nit.value_bet_top = 0.22;
        nit.bluff_mult = 0.25;
        nit.draw_bet = 0.15;
        nit.slowplay = 0.30;
        nit.value_size_shift = -1;

        ArchetypeParams& station = t[2];
        station.open_width = 1.40;
        station.max_range_pct = 80.0;
        station.limp_pct = 20.0;
        station.limp_trap = 0.6;
        station.flat_width = 2.20;
        station.three_bet_width = 0.4;
        station.call_3bet_width = 2.0;
        station.four_bet_width = 0.5;
        station.continue_mult = 1.40;
        station.continue_floor = 0.35;
        station.pairs_and_draws_continue = true;
        station.raise_share = 0.03;
        station.value_bet_top = 0.25;
        station.bluff_mult = 0.4;
        station.draw_bet = 0.2;
        station.slowplay = 0.55;
        station.value_size_shift = 1;

        ArchetypeParams& maniac = t[3];
        maniac.open_width = 1.80;
        maniac.max_range_pct = 90.0;
        maniac.limp_pct = 4.0;
        maniac.limp_trap = 0.1;
        maniac.flat_width = 1.20;
        maniac.three_bet_width = 2.5;
        maniac.call_3bet_width = 1.3;
        maniac.four_bet_width = 2.0;
        maniac.continue_mult = 1.05;
        maniac.continue_floor = 0.15;
        maniac.raise_share = 0.25;
        maniac.value_bet_top = 0.45;
        maniac.bluff_mult = 1.8;
        maniac.draw_bet = 0.8;
        maniac.slowplay = 0.10;
        return t;
    }();
    return table[static_cast<std::size_t>(archetype)];
}

// --- Board strength -------------------------------------------------------------

StreetStrength street_strength(std::span<const Card> board) {
    if (board.size() < 3 || board.size() > 5) throw std::invalid_argument("street_strength: board needs 3 to 5 cards");
    const CardMask board_mask = card_mask(board);
    for (const Card c : board) {
        if (c.id >= kDeckSize) throw std::invalid_argument("street_strength: invalid card");
    }
    if (std::popcount(board_mask) != static_cast<int>(board.size())) {
        throw std::invalid_argument("street_strength: duplicate board card");
    }

    StreetStrength s;
    s.board_size = static_cast<int>(board.size());
    std::array<HandRank, kComboCount> rank{};
    for (int combo = 0; combo < kComboCount; ++combo) {
        if ((combo_mask(combo) & board_mask) != 0) continue;
        const auto i = static_cast<std::size_t>(combo);
        const HandRead read = read_hand(combo_cards(combo), board);
        rank[i] = read.rank;
        s.draw[i] = read.strong_draw() ? kStrongDraw : read.gutshot ? kGutshotOnly : kNoDraw;
        s.holds_pair.set(i, read.holds_pair);
        s.live.set(i);
        s.order[static_cast<std::size_t>(s.live_count++)] = static_cast<std::uint16_t>(combo);
    }
    const auto live = s.order.begin() + s.live_count;
    std::sort(s.order.begin(), live, [&rank](std::uint16_t a, std::uint16_t b) {
        return rank[a] != rank[b] ? rank[a] < rank[b] : a < b;
    });

    // Lower rank is stronger, so the combos after a tie group are the ones it beats.
    const int n = s.live_count;
    for (int start = 0; start < n;) {
        int end = start;
        const HandRank r = rank[s.order[static_cast<std::size_t>(start)]];
        while (end < n && rank[s.order[static_cast<std::size_t>(end)]] == r) ++end;
        const double beaten = n - end;
        const double tied = end - start - 1;
        const auto value = static_cast<float>(n > 1 ? (beaten + 0.5 * tied) / (n - 1) : 0.5);
        for (int k = start; k < end; ++k) s.hs[s.order[static_cast<std::size_t>(k)]] = value;
        start = end;
    }
    return s;
}

std::array<float, kComboCount> top_share(const WeightedRange& range, const StreetStrength& strength) {
    std::array<float, kComboCount> out;
    out.fill(1.0f);
    double total = live_total(range, strength);
    const bool uniform = total <= 0.0;
    if (uniform) total = strength.live_count;
    const auto weight = [&](std::uint16_t combo) { return uniform ? 1.0 : static_cast<double>(range.weight(combo)); };

    const std::span<const std::uint16_t> order = strength.live_order();
    double above = 0.0;
    for (std::size_t start = 0; start < order.size();) {
        std::size_t end = start;
        double group = 0.0;
        while (end < order.size() && strength.hs[order[end]] == strength.hs[order[start]]) group += weight(order[end++]);
        const auto value = static_cast<float>(total > 0.0 ? (above + 0.5 * group) / total : 1.0);
        for (std::size_t k = start; k < end; ++k) out[order[k]] = value;
        above += group;
        start = end;
    }
    return out;
}

// --- Postflop likelihoods -------------------------------------------------------

double BetBreakdown::bet_share() const {
    return range_mass > 0.0 ? (value_mass + draw_mass + medium_mass + air_mass) / range_mass : 0.0;
}

double BetBreakdown::air_share_of_polarised() const {
    const double polarised = value_mass + air_mass;
    return polarised > 0.0 ? air_mass / polarised : 0.0;
}

BetBreakdown bet_breakdown(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                           const ArchetypeParams& params, const WeightedRange* opponent) {
    const OwnedFooting f(range, opponent, strength);
    return bet_breakdown_at(range, strength, f.footing(), safe_size(size_pots), params);
}

WeightedRange bet_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                             const ArchetypeParams& params, const WeightedRange* opponent) {
    return bet_breakdown(range, strength, size_pots, params, opponent).likelihood;
}

WeightedRange check_likelihood(const WeightedRange& range, const StreetStrength& strength,
                               const ArchetypeParams& params, const WeightedRange* opponent) {
    const OwnedFooting f(range, opponent, strength);
    return check_likelihood_at(range, strength, f.footing(), 1.0, params);
}

WeightedRange call_likelihood(const WeightedRange& range, const StreetStrength& strength, Street street,
                              double size_pots, double pot_odds, const ArchetypeParams& params,
                              const WeightedRange* bettor) {
    const OwnedFooting f(range, bettor, strength);
    return call_likelihood_at(range, strength, f.footing(), street, safe_size(size_pots), pot_odds, params);
}

WeightedRange raise_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                               const ArchetypeParams& params, const WeightedRange* bettor) {
    const OwnedFooting f(range, bettor, strength);
    return raise_likelihood_at(range, strength, f.footing(), safe_size(size_pots), params);
}

Response predict_postflop_response(const WeightedRange& range, const StreetStrength& strength, Street street,
                                   double size_pots, double pot_odds, const ArchetypeParams& params,
                                   const WeightedRange* bettor_range) {
    const OwnedFooting f(range, bettor_range, strength);
    return postflop_response_at(range, strength, f.footing(), street, safe_size(size_pots), pot_odds, params);
}

Response predict_preflop_response(const WeightedRange& villain_range_so_far, Position villain, Position raiser,
                                  int raises_after_hero, double size_ratio, int players,
                                  const ArchetypeParams& params, bool villain_limped) {
    const double scale = std::isfinite(size_ratio) && size_ratio > 0.0 ? 1.0 / std::sqrt(size_ratio) : 1.0;
    const int level = std::max(1, raises_after_hero);
    const ChartAnswer answer = villain_limped && level == 1
                                   ? limper_answer(villain, players, scale, params)
                                   : facing_raise_answer(villain, raiser, level, players, scale, params);
    Response out;
    double mass = 0.0;
    double calls = 0.0;
    double raises = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const double w = villain_range_so_far.weight(combo);
        const double call = answer.call.weight(combo);
        const double raise = answer.raise.weight(combo);
        mass += w;
        calls += w * call;
        raises += w * raise;
        out.call_range.set(combo, static_cast<float>(w * call));
        out.raise_range.set(combo, static_cast<float>(w * raise));
    }
    if (mass > 0.0) {
        out.call = calls / mass;
        out.raise = raises / mass;
        out.fold = std::max(0.0, 1.0 - out.call - out.raise);
    }
    return out;
}

// --- Ranges from a view ---------------------------------------------------------

std::array<std::optional<WeightedRange>, kMaxSeats> infer_ranges(const PlayerView& view, Archetype archetype) {
    return infer_ranges(view, archetype_params(archetype));
}

std::array<std::optional<WeightedRange>, kMaxSeats> infer_ranges(const PlayerView& view,
                                                                 const ArchetypeParams& params) {
    std::uint8_t wanted = 0;
    for (int i = 0; i < kMaxSeats; ++i) {
        const PublicSeat& s = view.seats[static_cast<std::size_t>(i)];
        const bool contesting = s.status == SeatStatus::Active || s.status == SeatStatus::AllIn;
        if (i != view.seat && contesting && s.position) wanted = static_cast<std::uint8_t>(wanted | (1u << i));
    }
    RangeWalk walk(view, params, true);
    walk.run();
    std::array<std::optional<WeightedRange>, kMaxSeats> out{};
    for (int i = 0; i < kMaxSeats; ++i) {
        if (((wanted >> i) & 1u) != 0) out[static_cast<std::size_t>(i)] = walk.range(i);
    }
    return out;
}

WeightedRange infer_range_for(const PlayerView& view, int seat_index, Archetype archetype, bool remove_viewer_cards) {
    return infer_range_for(view, seat_index, archetype_params(archetype), remove_viewer_cards);
}

WeightedRange infer_range_for(const PlayerView& view, int seat_index, const ArchetypeParams& params,
                              bool remove_viewer_cards) {
    if (seat_index < 0 || seat_index >= kMaxSeats) throw std::invalid_argument("infer_range_for: bad seat index");
    if (!view.seats[static_cast<std::size_t>(seat_index)].position) return WeightedRange{};
    // The viewer's cards are never removed from the viewer's own range.
    const bool remove = remove_viewer_cards && seat_index != view.seat;
    RangeWalk walk(view, params, remove);
    walk.run();
    return walk.range(seat_index);
}

}  // namespace poker
