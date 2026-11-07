#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/card.h"
#include "engine/player_view.h"
#include "engine/table_types.h"
#include "range/combos.h"
#include "range/weighted_range.h"

namespace poker {

// Estimated hand ranges for the players at a table, read from public actions alone,
// and the same model run forwards to predict how a range answers a bet. It is a
// heuristic opponent model for the advisor, not a solver: every number below is an
// assumption about how people play, chosen to be plausible rather than optimal.
//
// A range is a weight per combo. Each action a player takes multiplies its range by
// that action's likelihood, P(action | combo), so a range is the product over the
// player's actions, starting from every combo at weight 1. Combos holding a known
// card (the board, and the viewer's own cards where asked) are zeroed.
//
// The walk over a view tracks every dealt player's range, the viewer's included,
// because a bet or raise is judged against the other players' ranges (below). All
// of them are modeled with the one set of params given. With remove_viewer_cards
// the viewer's cards are removed from the other players' ranges from the start, so
// every likelihood is computed on ranges conditioned on them (the viewer's own range
// never loses them). Measured against removing them only at the end, this moves
// the viewer's equity by at most 1.5 pp in the spots tried.
//
// Layering: read_hand (made hand, pair, draws) comes from bots/gto_heuristic_engine.h,
// so this module depends on the bots' hand reader, though never on bot behaviour.
//
// --- Player types -------------------------------------------------------------
//
// Four generic archetypes, applied to every opponent alike: Baseline (roughly the
// charts and balanced play), Nit (tight, folds a lot), Station (calls too much) and
// Maniac (bets and raises too much). ArchetypeParams holds the knobs; the functions
// that take an Archetype also take explicit params, for tuning.
//
// --- Preflop --------------------------------------------------------------------
//
// Positions come from the public table; `players` is the number dealt in. k counts
// the raises before the action: a raise is a new level only if its increment is
// at least the last full raise's (the engine's rule), so a short all-in does not
// count and a later re-raise reads as it would without it; the first raise always
// counts. resize(R, m) is resize_range(R, m, cap) with cap 100 for m <= 1 and
// max(max_range_pct, R's own %) for m > 1: the cap limits growth only, and never
// below the chart itself, so a type narrower than the charts is narrower by its
// width alone. sp is `slowplay`.
//
//   Open (k = 0, no limpers)      resize(rfi_range(pos), open_width) at 1
//   Iso-raise over limpers        resize(rfi_range(pos), 0.75 * open_width); from the
//                                 BB, its raise range (below)
//   Limp, or SB completes         the window: the strongest limp_pct % of combos
//                                 outside the open range, at 1, plus the premium
//                                 tier at limp_trap
//   BB checks its option          1 for everything outside the BB raise range
//                                 (resize(premium and strong tiers, open_width)),
//                                 sp on it. Not capped: the check is free.
//   A limper facing the first     raise: 1 - sp on the premiums outside the window
//   raise (limped earlier)        (the traps) and on the strongest
//                                 0.1 x three_bet_width of the window; call: sp
//                                 there, 1 on the strongest 0.6 x flat_width of the
//                                 window (fractions of the window's combos, by
//                                 strength). The charts do not apply: their call
//                                 ranges barely meet a limp range.
//   Facing one raise              chart = advisor_vs_open_ranges(pos, opener) (not
//                                 the bots' vs_open_ranges), with raise width
//                                 three_bet_width, call width flat_width
//   Facing a 3-bet, as the        chart = vs_three_bet_ranges(pos, 3-bettor),
//   opener                        four_bet_width and call_3bet_width
//   Facing a 4-bet, as the        chart = vs_four_bet_ranges(pos, 4-bettor),
//   3-bettor                      four_bet_width and call_3bet_width
//   Any other player facing a     chart = the non-positional vs-3-bet ranges (a cold
//   3-bet, or raise #3 or later   caller squeezed, a cold 4-bet) or vs-4-bet ranges,
//                                 the same widths
//
// For a chart and its widths, R3 = resize(chart.raise, raise width) and
// Cc = resize(chart.call, call width). Then per class:
//   raise: 1 - sp in R3, else 0
//   call:  sp in R3; 1 in Cc or in chart.raise (raises the type does not make
//          become calls); else 0
// so raise + call <= 1 for every class, and the same numbers serve as the
// prediction in predict_preflop_response. The SB's charts have no call range, so
// a SB call uses the in-position call range (advisor_vs_open_ranges(BTN, opener)).
//
// A call (not all in and not committing) also responds to the raise's size, as
// predict_preflop_response's calls do (below): with f = call_size_scale of the
// latest raise's size_ratio (standard_preflop_raise_to), Cc is resized by
// min(1, f), and below the standard size the range so far gains calls by
// (f - 1) x its call mass. A limper's call scales its call width by f the same
// way. So a flat of a min-raise reads wider than a flat of a standard open, and
// inference and prediction agree; the raises do not depend on size.
//
// Stack-size overrides, checked before the table above. A raise "commits" when it
// is all in or reaches 40% of the stack the player started the hand with; its
// depth D is the effective stack in big blinds: the smaller of what the raiser
// commits (its street total when all in, else its starting stack) and the deepest
// starting stack among the other players still in. For D <= 25 BB the push/fold
// tables (strategy/push_fold.h) apply at the table stack clamp(round(min(D, 15)),
// 3, 15), and from 15 to 25 BB they blend into the deep commit ranges: with
// t = clamp((D - 15) / 10, 0, 1), a likelihood is (1 - t) x the push/fold answer
// + t x the commit answer, so there is no cliff at 15 or 16 BB.
//   Committing raise, first in    push_fold_jam_range(pos) at open_width (the BB's
//                                 raise over limps uses the SB's jam range)
//   Committing re-raise (facing   the call range against a jam from the last
//   a raise)                      raiser, at three_bet_width: a re-jam is taken to
//                                 be what would call a jam of that size
//   Call of an all-in             the call range against a jam from the all-in
//                                 player, at flat_width
//   Call ranges against a jam come from push_fold_call_range(caller, jammer); a
//   caller who acted before the jammer (the opener facing a re-jam) has no table
//   and uses the BB's call range against that jammer, or the BB's against the SB
//   when the jammer is the BB. Every multiway nuance is left out: an overcall uses
//   the same range as a lone call.
//   Deeper than 25 BB:
//   Any committing raise          the commit range, resize(vs-4-bet raise + call,
//                                 four_bet_width) at 1
//   A committing call: over 10 BB, and either at least 50 BB or at least a third
//   of what is at stake (the same test as the bots' charts)
//       resize(commit range, call_3bet_width) at 1
//
// --- Postflop -------------------------------------------------------------------
//
// StreetStrength ranks every combo on the board as it stands. For a range, a
// combo's top_share t is its position in that range from the strongest hand down
// (0 = the nuts, 1 = the worst). Sizes s are fractions of the pot after calling,
// measured from the action records: the pot before an action is
// street_start_pot plus every chip put in earlier on that street, so a bet is
// chips / pot and a raise is (street total - bet faced) / (pot + chips the
// raiser needed to call).
//
// edge(x, t) is a smooth step on t around the threshold x with width w =
// softness: 1 for t <= x - 2w, 0 for t >= x + 2w, 0.5 at x, and the cubic
// 3y^2 - 2y^3 between, y = (x - t) / 4w + 1/2. Nothing leaks past 2w: with a
// logistic, the tails over many weak hands outweighed a narrow value region.
//
// Standing. Bets and raises are judged against the opponents, not the actor's own
// range. A combo's standing u is the largest top_share it holds in any live
// opponent's current range (for a raise the bettor's range includes its bet):
// u <= x means it beats at least 1 - x of every opponent's range, ties counting
// half, the combo's own cards ignored. A value bet must beat whoever calls, so
// multiway the worst standing counts. Value means more than half the equity
// against the hands that call: if the callers are the top C of a range, that is
// u <= C / 2. C is a standard player's continue share whatever the types,
// Cref(s) = clamp(0.95 / (1 + s), 0.10, 0.95), so a type's aggression enters only
// through the shift a = value_bet_top - 0.33 (Baseline -0.05, Maniac +0.12, Nit
// -0.11).
//
// Own-range rule: the public functions given no opponent range (a null pointer)
// read u as t and use the old thresholds, value_bet_top x (1.6 for s <= 0.4, 1.0
// for s <= 0.8, else 0.75) for bets and value_bet_top / 2 for raises, with air
// raising at 0.1 x bluff_mult and no barrel, slowplay or bluff-cap terms. It
// judges value within the actor's range alone, so it overstates value raises from
// a range facing a strong bettor; the walk never uses it while an opponent is in.
//
// Bet of size s, for a player not facing a bet:
//   V    = Cref(s) / 2 + a, times 0.6 on the river (Cref(s) x 0.3 + a: ahead of
//          70% of the calls, since a river bet is called only by hands that can
//          beat it or catch bluffs)
//   p_v  = edge(V, u) * (1 - sp x min(1, s / 0.5))   value; a bet under half pot
//          keeps the opponent's range in, so value slowplays less
//   x    = draw_bet for a strong draw (cards to come)  semi-bluff
//          b for u > 0.6 without a strong draw          air
//          0.15 * value_bet_top / 0.33 otherwise        medium hands
//   P    = p_v + (1 - p_v) * x
// b calibrates the bluffs: value mass M_v = sum(range * p_v), air mass
// M_a = b * sum over air of range * (1 - p_v), and b = min(0.9, value) solving
// M_a = g * bluff_mult * s / (1 + s) * M_v, where g = 4 for the previous street's
// aggressor on the flop or turn (a c-bet or barrel: its air has equity and its
// story fold equity) and 1 otherwise. While b < 0.9 the bluff share of the
// polarised part, M_a / (M_a + M_v), is m * r / (1 + m * r) with r = s / (1 + s),
// m = g * bluff_mult: s / (1 + 2s) at bluff_mult 1 on the river, the toy game's
// indifference share (the Baseline's 1.2 bluffs a little more). The cap 0.9
// keeps some air checking, so a check never removes air outright. Draws and
// medium hands bet on top of that and are not counted as either part.
//
// Check, not facing a bet:  1 - P(bet of 0.5 pot). Value hands keep sp x (1 -
// the medium rate) (sp, then the medium rate's bet on the rest). Checking to the
// aggressor (the last to bet or raise on the previous street, still in and yet to
// act on this one) is what nearly every hand does, so it narrows only by
// 1 - lead_share * P(bet of 0.5 pot). Without this the BB's check to the preflop
// raiser would strip most of its strong hands.
//
// Continue facing a bet of size s with pot odds o (within the caller's own range:
// defending the top of one's range is the standard model):
//   C    = clamp(continue_mult * MDF, continue_floor, 0.95), MDF = 1 / (1 + s)
//   p_c  = edge(C, t)
//   strong draws: max(p_c, 1 if draw equity >= 0.9 * o, else 0.3), where draw
//        equity is 0.35 on the flop and 0.18 on the turn
//   pairs_and_draws_continue (Station): max(p_c, pairs_and_draws_prob) for a
//        hand holding a pair (read_hand's holds_pair) or any draw
//
// Raise facing a bet of size s. The raise's own size is ignored: the bettor is
// assumed to face a raise to three times its bet, s_r = 2s / (1 + 2s) pots.
//   V_r  = 0.5 x Cref(s_r) / 2 + a: players fold more to raises than to bets, so
//          the bettor is taken to continue with the top 0.5 x Cref(s_r) of its
//          range, and a value raise must be ahead of half of that
//   p_rv = edge(V_r, u) * (1 - sp)
//   y    = draw_bet / 2 for a strong draw, b_r for air (t > 0.6 in the raiser's
//          own range: bluff raises come from its weakest hands), else 0
//   b_r  = min(1, bluff_mult * s_r / (1 + s_r) * M_rv / air room), the bet's
//          calibration with M_rv = sum(range * p_rv)
//   q    = p_rv + (1 - p_rv) * y
//   R    = raise_share * clamp(1.5 / (1 + s), 0.4, 1.25)   (raise_share_scale)
//   P_raise = k * q with k = min(1, R / Q), Q = the range's mean q: R caps the
//        share of the range that raises, scaling every combo alike so the shape
//        holds. The cap is raise_share at half pot and falls as the bet grows:
//        x1.13 at 0.33 pot, x0.86 at 0.75, x0.67 at 1.25, x0.5 at 2 pots, and
//        never below x0.4. The inference likelihood and the predicted response
//        use the same cap for the same faced size.
//
// Bluff raises come out of the continue budget: with F = the range's mean of
// (1 - p_c) * P_raise, the hands that would fold but raise, p_c is recomputed
// with C - F in place of C (one step), so about C of the range continues in all
// before draws and the Station rule add to it.
//
// Then P_call = p_c * (1 - P_raise) and P_fold = (1 - p_c) * (1 - P_raise), which
// sum to 1 with P_raise. The same functions predict a range's response to a bet,
// so narrowing a range and predicting fold equity stay consistent.
//
// --- Fallback -------------------------------------------------------------------
//
// A product is trusted if it holds at least 1% of all combos (13.26) or at least
// 0.5% of the range before the action (well under any raise share, so a Station's
// or a Nit's raise keeps its product). Otherwise the action was one the model all
// but ruled out, and its likelihood is put on the range as the current street
// began (if that product is trusted by the same test), else on the range as
// preflop ended (if not empty), else on every combo, with known cards removed. If
// all are empty, the action is ignored. So a hand folded preflop never comes back
// postflop.

enum class Archetype : std::uint8_t { Baseline, Nit, Station, Maniac };

inline constexpr int kArchetypeCount = 4;

constexpr std::string_view to_string(Archetype archetype) {
    constexpr std::array<std::string_view, kArchetypeCount> names{"Baseline", "Nit", "Station", "Maniac"};
    return names[static_cast<std::size_t>(archetype)];
}

// The defaults are Baseline's. Its continue_mult, value_bet_top and bluff_mult
// (0.85, 0.28, 1.2; first 0.95, 0.33, 1.0) come from Stage 5's one-round grid
// search on the river bench (docs/advisor-upgrade-notes.md, "Stage 5 tuning"):
// tuned on hand-written bench ranges, so a fit to assumptions, not to play.
struct ArchetypeParams {
    // Preflop widths, as multiples of the charts' combos.
    double open_width = 1.00;
    double max_range_pct = 100.0;  // Growth beyond a chart stops at max(this, the chart's %)
    double limp_pct = 2.0;         // Limps: this % of combos just outside the opens
    double limp_trap = 0.3;        // ... plus premiums at this weight
    double flat_width = 1.00;
    double three_bet_width = 1.00;
    double call_3bet_width = 1.00;
    double four_bet_width = 1.00;

    // Postflop.
    double continue_mult = 0.85;   // Continue share facing a bet = this x MDF ...
    double continue_floor = 0.10;  // ... at least this, at most 0.95
    bool pairs_and_draws_continue = false;
    double pairs_and_draws_prob = 0.9;
    double raise_share = 0.10;     // Most of a range that raises a half-pot bet (see raise_share_scale)
    double value_bet_top = 0.28;   // Value thresholds shift by this - 0.33 (own-range rule: the top share)
    double bluff_mult = 1.2;       // Air : value = bluff_mult * s / (1 + s), x4 when barrelling
    double draw_bet = 0.5;         // Strong draws bet this often
    double slowplay = 0.20;        // Value hands that check or flat instead
    double softness = 0.06;        // Edge width w, in top_share: each edge spans 4w
    double lead_share = 0.15;      // Leads into the aggressor, as a share of P(bet)
    int value_size_shift = 0;      // Ladder rungs to shift value bets (used by the advisor)
};

const ArchetypeParams& archetype_params(Archetype archetype);

// --- Board strength -------------------------------------------------------------

inline constexpr std::uint8_t kNoDraw = 0;
inline constexpr std::uint8_t kGutshotOnly = 1;
inline constexpr std::uint8_t kStrongDraw = 2;  // Flush draw or open-ended

// Every combo's made-hand strength on one board of 3 to 5 cards. Live combos are
// those holding no board card; every array entry for a dead combo is 0.
struct StreetStrength {
    int board_size = 0;
    // Share of the other live combos this one beats now, ties counting half, in
    // [0, 1]. It ignores the combo's own cards blocking others, so equal hands
    // get exactly equal values.
    std::array<float, kComboCount> hs{};
    // kNoDraw, kGutshotOnly or kStrongDraw, from read_hand: only with cards to come.
    std::array<std::uint8_t, kComboCount> draw{};
    std::bitset<kComboCount> holds_pair;  // read_hand's holds_pair
    std::bitset<kComboCount> live;
    // The live combos, strongest first, ties by combo index: the first
    // live_count entries.
    std::array<std::uint16_t, kComboCount> order{};
    int live_count = 0;

    std::span<const std::uint16_t> live_order() const {
        return {order.data(), static_cast<std::size_t>(live_count)};
    }
};

// One hand evaluation per live combo plus a sort. Throws std::invalid_argument
// unless the board has 3 to 5 distinct cards.
StreetStrength street_strength(std::span<const Card> board);

// Each combo's position in `range`, strongest first along strength.order: the
// weight of every stronger combo plus half the weight of its own tie group (the
// midpoint), over the range's live total. Equal hands share one value. A combo
// with no weight still gets the position it would hold. Dead combos get 1. An
// empty range is read as uniform over the live combos. O(1326).
std::array<float, kComboCount> top_share(const WeightedRange& range, const StreetStrength& strength);

// --- Postflop likelihoods (see the model above) ---------------------------------

// Where a range's bet comes from: masses of range * P(bet) by reason.
struct BetBreakdown {
    WeightedRange likelihood;  // P(bet | combo), 0 for dead combos
    double range_mass = 0.0;   // The range's live total
    double value_mass = 0.0;   // sum(range * p_v)
    double draw_mass = 0.0;    // Strong draws' (1 - p_v) * draw_bet part
    double medium_mass = 0.0;  // Medium hands' (1 - p_v) * x part
    double air_mass = 0.0;     // Air's (1 - p_v) * b part: the bluffs
    double bluff_weight = 0.0; // b

    // Share of the range that bets.
    double bet_share() const;
    // Bluffs over bluffs plus value: m * r / (1 + m * r) while b < 1.
    double air_share_of_polarised() const;
};

// Not facing a bet: a bet of `size_pots`. `opponent` is the range the bet is
// judged against (value means beating enough of it); null applies the own-range
// rule (see the model above).
BetBreakdown bet_breakdown(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                           const ArchetypeParams& params, const WeightedRange* opponent = nullptr);
WeightedRange bet_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                             const ArchetypeParams& params, const WeightedRange* opponent = nullptr);
// A check not to the aggressor (the full 1 - P(bet)).
WeightedRange check_likelihood(const WeightedRange& range, const StreetStrength& strength,
                               const ArchetypeParams& params, const WeightedRange* opponent = nullptr);

// Facing a bet of `size_pots` at `pot_odds` (call / (pot + call)). `bettor` is the
// bettor's range after its bet, which value raises are judged against; null
// applies the own-range rule. Calls depend on it too, through the raises.
WeightedRange call_likelihood(const WeightedRange& range, const StreetStrength& strength, Street street,
                              double size_pots, double pot_odds, const ArchetypeParams& params,
                              const WeightedRange* bettor = nullptr);
// A raise of a bet of `size_pots`: the size sets the raise share cap.
WeightedRange raise_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                               const ArchetypeParams& params, const WeightedRange* bettor = nullptr);

// The factor on raise_share for a faced bet of `size_pots` (a fraction of the pot
// after calling): clamp(1.5 / (1 + size_pots), 0.4, 1.25). A non-finite or
// negative size counts as 0.
double raise_share_scale(double size_pots);

// A range's answer to a bet or raise: shares by mass (summing to 1 for a range
// with live weight; an empty range folds), and the ranges that call and raise.
struct Response {
    double fold = 1.0;
    double call = 0.0;
    double raise = 0.0;
    WeightedRange call_range;
    WeightedRange raise_range;
};

// `range` facing a bet of `size_pots` at `pot_odds`, on the street `strength` is
// for. `bettor_range` is the bettor's range including this bet (for the advisor,
// Hero's range as the villain sees it), which the villain's value raises are
// judged against; null applies the own-range rule, which reads value raises from
// the villain's own range alone and overstates them against a strong bettor.
Response predict_postflop_response(const WeightedRange& range, const StreetStrength& strength, Street street,
                                   double size_pots, double pot_odds, const ArchetypeParams& params,
                                   const WeightedRange* bettor_range = nullptr);

// A preflop spot for predict_preflop_response: `villain` answering the latest
// raise, or, with raises 0, a limp.
struct PreflopSpot {
    Position villain = Position::BB;
    Position raiser = Position::BTN;  // Who made the latest raise (for an open, the opener)
    // Raise levels including the latest: 1 an open, 2 a 3-bet, 3 or more a 4-bet or
    // more; 0 a limped pot with no raise, answered as the inference reads it (the BB
    // raises with its raise range and checks the rest; anyone else iso-raises with
    // 0.75 x open_width of its open range, limps behind with the limp window, or folds).
    int raises = 1;
    // The latest raise's street total over a standard one's.
    double size_ratio = 1.0;
    int players = kMaxSeats;
    bool villain_limped = false;  // Called before any raise: the limper's answer to a first raise
    // The villain made the level below the latest (the opener facing a 3-bet, the
    // 3-bettor facing a 4-bet): the positional tables. False for cold callers and
    // players yet to act, who answer from the non-positional ones.
    bool villain_raised_last = true;
    // The latest raise commits the raiser (all in, or at least 40% of its starting
    // stack), with this effective stack in big blinds: the push/fold override below.
    std::optional<double> all_in_bb;
};

// The standard street total for the next preflop raise after `raises` raise
// records (every raise, short all-ins included) and `callers` calls, with `current`
// the bet to match: 2.5 big blinds plus one per caller first in; against one raise
// `current` x (3 + callers) in position and x (3.5 + callers) out of position (a
// 3-bettor who acts before the opener after the flop, as the blinds do), the sizes
// the positional vs-3-bet tables assume (preflop_charts.h); 2.3 x `current`
// against two; and 0 after that (all in). The chart bots' preflop_size
// (gto_heuristic_engine.cpp) is the same but 3x from every position. A raise's
// size_ratio is its street total over this, computed before it was made, which
// the inference and the advisor both use.
double standard_preflop_raise_to(int big_blind, int raises, int callers, int current, bool out_of_position = false);

// `villain_range_so_far` facing the spot's raise. The chart and widths are the
// inference's for that spot (see above). The calls also respond to the raise's
// size, by f = size_ratio^-0.5 for a raise at least the standard size and
// f = size_ratio^-1 below it. Above the standard size Cc is resize(call chart,
// call width x f), so bigger raises lose their weakest calls first. Below it the
// range gains calls worth (f - 1) x its call mass, from the strongest classes of
// villain_range_so_far that the chart folds (outside R3, Cc and chart.raise), each
// added while the mass before it plus half its own is within that target, and
// while the classes that continue stay within max(max_range_pct, their %) of all
// combos. (Growing Cc itself, as before Stage 6, mostly added hands already in the
// raise chart, so call shares were flat below the standard size.) So a min-raise
// or a min-3-bet is called more often than a standard one; the raises are
// unchanged. Measured for Baseline: the BB calls a BTN open of 1.5, 2 and 2.5 BB
// 79.3, 59.1 and 47.3% of the time (plus the same 3-bets), and a CO open faces
// a BB 3-bet of 0.53x, 1x and 1.4x the standard size with 68.2, 36.3 and 30.9%
// calls. The inference reads a call with the same rule. With villain_limped and a first raise, the limper's answer (above)
// replaces the chart, its call width scaled by f.
//
// With all_in_bb at most 25 BB, the villain calls with the push/fold call range
// against a jam from the raiser at the table stack (resized by flat_width, as in
// the inference) and never raises; from 15 to 25 BB that answer blends, by the t
// above, into the chart answer for the spot (whose raises then count as calls of
// the all-in for the advisor). Deeper all-ins use the chart answer alone, as do
// the inference's other stack-size overrides, which are not applied here.
Response predict_preflop_response(const WeightedRange& villain_range_so_far, const PreflopSpot& spot,
                                  const ArchetypeParams& params);

// The same, for a raise that is not all in, with villain_raised_last true:
// raises_after_hero below 1 counts as 1.
Response predict_preflop_response(const WeightedRange& villain_range_so_far, Position villain, Position raiser,
                                  int raises_after_hero, double size_ratio, int players,
                                  const ArchetypeParams& params, bool villain_limped = false);

// --- Ranges from a view ---------------------------------------------------------

// A range for every other player still contesting the pot (Active or AllIn) and
// dealt in; nullopt for the viewer and for folded, busted or undealt players. The
// board and the viewer's own cards are removed, the viewer's from the start, so
// the narrowing is conditioned on them.
std::array<std::optional<WeightedRange>, kMaxSeats> infer_ranges(const PlayerView& view, Archetype archetype);
std::array<std::optional<WeightedRange>, kMaxSeats> infer_ranges(const PlayerView& view,
                                                                 const ArchetypeParams& params);

// The range of the player at `seat_index`, the viewer included (Stage 8 uses that
// for Hero's range as opponents see it). The board is always removed, and the
// viewer's own cards only with remove_viewer_cards (never for the viewer's own
// range). A folded player's range is as it stood before the fold; a player not
// dealt in gets an empty range. Throws std::invalid_argument for a seat index
// outside [0, kMaxSeats).
WeightedRange infer_range_for(const PlayerView& view, int seat_index, Archetype archetype,
                              bool remove_viewer_cards);
WeightedRange infer_range_for(const PlayerView& view, int seat_index, const ArchetypeParams& params,
                              bool remove_viewer_cards);

}  // namespace poker
