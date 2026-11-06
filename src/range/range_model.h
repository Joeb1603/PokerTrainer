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
// the raises before the action. resize(R, m) is resize_range(R, m, max_range_pct),
// so every voluntary range is capped at max_range_pct; sp is `slowplay`.
//
//   Open (k = 0, no limpers)      resize(rfi_range(pos), open_width) at 1
//   Iso-raise over limpers        resize(rfi_range(pos), 0.75 * open_width); from the
//                                 BB, its raise range (below)
//   Limp, or SB completes         a window of limp_pct % of combos: the strongest
//                                 classes outside the open range, at 1, plus the
//                                 premium tier at limp_trap
//   BB checks its option          1 for everything outside the BB raise range
//                                 (resize(premium and strong tiers, open_width)),
//                                 sp on it. Not capped: the check is free.
//   Facing one raise              chart = vs_open_ranges(pos, opener), with
//                                 raise width three_bet_width, call width flat_width
//   Facing a 3-bet                chart = the vs-3-bet ranges, four_bet_width and
//                                 call_3bet_width (non-positional until Stage 6)
//   Facing raise #3 or later      chart = the vs-4-bet ranges, four_bet_width and
//                                 call_3bet_width
//
// For a chart and its widths, R3 = resize(chart.raise, raise width) and
// Cc = resize(chart.call, call width). Then per class:
//   raise: 1 - sp in R3, else 0
//   call:  sp in R3; 1 in Cc or in chart.raise (raises the type does not make
//          become calls); else 0
// so raise + call <= 1 for every class, and the same numbers serve as the
// prediction in predict_preflop_response. The SB's charts have no call range, so
// a SB call uses the in-position call range (vs_open_ranges(BTN, opener)).
//
// Stack-size overrides, checked before the table above:
//   All-in raise of at most 15 BB (street total)
//       TEMPORARY, until Stage 6's push/fold tables: the top
//       clamp(400 / bb, 5, 100) % by strength, halved when facing a raise, times
//       open_width, capped. 10 BB first in is the top 40%, 15 BB 27%, 5 BB 80%.
//   Call of an all-in of at most 15 BB
//       TEMPORARY as above: the top clamp(250 / bb, 8, 100) % times flat_width,
//       capped, with bb the all-in's street total in big blinds.
//   Any other all-in raise (so over 15 BB; the plan's 20 BB rule is covered too),
//   or a raise to at least 40% of the stack the player started the hand with
//       the commit range, resize(vs-4-bet raise + call, four_bet_width) at 1
//   A committing call: over 10 BB, and either at least 50 BB or at least a third
//   of what is at stake (the same test as the bots' charts)
//       resize(commit range, call_3bet_width) at 1
//
// --- Postflop -------------------------------------------------------------------
//
// StreetStrength ranks every combo on the board as it stands. For a range, a
// combo's top_share t is its position in that range from the strongest hand down
// (0 = the nuts, 1 = the worst). sigma(x) = 1 / (1 + e^-x) and w = softness.
// Sizes s are fractions of the pot after calling, measured from the action
// records: the pot before an action is street_start_pot plus every chip put in
// earlier on that street, so a bet is chips / pot and a raise is
// (street total - bet faced) / (pot + chips the raiser needed to call).
//
// Bet of size s, for a player not facing a bet:
//   V    = value_bet_top * (1.6 if s <= 0.4, 1.0 if s <= 0.8, else 0.75)
//   p_v  = sigma((V - t) / w) * (1 - sp)                      value
//   x    = draw_bet for a strong draw (cards to come)          semi-bluff
//          0.15 * value_bet_top / 0.33 for t <= 0.6            medium hands
//          b for t > 0.6 without a strong draw                 air
//   P    = p_v + (1 - p_v) * x
// b calibrates the bluffs: value mass M_v = sum(range * p_v), air mass
// M_a = b * sum over air of range * (1 - p_v), and b = min(1, value) solving
// M_a = bluff_mult * s / (1 + s) * M_v. So the bluff share of the polarised part,
// M_a / (M_a + M_v), is m * r / (1 + m * r) with r = s / (1 + s), m = bluff_mult:
// s / (1 + 2s) at m = 1, the river toy game's indifference share. Draws and medium
// hands bet on top of that and are not counted as either part.
//
// Check, not facing a bet:  1 - P(bet of 0.5 pot). Value hands keep at least sp.
// Checking to the aggressor (the last to bet or raise on the previous street,
// still in and yet to act on this one) is what nearly every hand does, so it
// narrows only by 1 - lead_share * P(bet of 0.5 pot). Without this the BB's check
// to the preflop raiser would strip most of its strong hands.
//
// Continue facing a bet of size s with pot odds o:
//   C    = clamp(continue_mult * MDF, continue_floor, 0.95), MDF = 1 / (1 + s)
//   p_c  = sigma((C - t) / w)
//   strong draws: max(p_c, 1 if draw equity >= 0.9 * o, else 0.3), where draw
//        equity is 0.35 on the flop and 0.18 on the turn
//   pairs_and_draws_continue (Station): max(p_c, pairs_and_draws_prob) for a
//        hand holding a pair (read_hand's holds_pair) or any draw
//
// Raise facing a bet of size s (the raise's own size is ignored):
//   p_rv = sigma((value_bet_top / 2 - t) / w) * (1 - sp)
//   y    = draw_bet / 2 for a strong draw, 0.1 * bluff_mult for air, else 0
//   q    = p_rv + (1 - p_rv) * min(1, y)
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
// If a product falls below 1% of all combos (13.26) and also below 5% of the
// range before the action, the action was one the model all but ruled out, so
// the range becomes that action's likelihood alone (with known cards removed). If
// that is empty too, the action is ignored. Legitimately narrow ranges, such as a
// Nit's 11-combo 4-bet range, keep their product.

enum class Archetype : std::uint8_t { Baseline, Nit, Station, Maniac };

inline constexpr int kArchetypeCount = 4;

constexpr std::string_view to_string(Archetype archetype) {
    constexpr std::array<std::string_view, kArchetypeCount> names{"Baseline", "Nit", "Station", "Maniac"};
    return names[static_cast<std::size_t>(archetype)];
}

// The defaults are Baseline's.
struct ArchetypeParams {
    // Preflop widths, as multiples of the charts' combos.
    double open_width = 1.00;
    double max_range_pct = 100.0;  // Cap on every voluntary range, % of combos
    double limp_pct = 2.0;         // Limps: this % of combos just outside the opens
    double limp_trap = 0.3;        // ... plus premiums at this weight
    double flat_width = 1.00;
    double three_bet_width = 1.00;
    double call_3bet_width = 1.00;
    double four_bet_width = 1.00;

    // Postflop.
    double continue_mult = 0.95;   // Continue share facing a bet = this x MDF ...
    double continue_floor = 0.10;  // ... at least this, at most 0.95
    bool pairs_and_draws_continue = false;
    double pairs_and_draws_prob = 0.9;
    double raise_share = 0.10;     // Most of a range that raises a half-pot bet (see raise_share_scale)
    double value_bet_top = 0.33;   // Top share of a range that bets for value at 0.5-0.8 pot
    double bluff_mult = 1.0;       // Air : value = bluff_mult * s / (1 + s)
    double draw_bet = 0.5;         // Strong draws bet this often
    double slowplay = 0.20;        // Value hands that check or flat instead
    double softness = 0.06;        // Width of the logistic edges, in top_share
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

// Not facing a bet: a bet of `size_pots`.
BetBreakdown bet_breakdown(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                           const ArchetypeParams& params);
WeightedRange bet_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                             const ArchetypeParams& params);
// A check not to the aggressor (the full 1 - P(bet)).
WeightedRange check_likelihood(const WeightedRange& range, const StreetStrength& strength,
                               const ArchetypeParams& params);

// Facing a bet of `size_pots` at `pot_odds` (call / (pot + call)).
WeightedRange call_likelihood(const WeightedRange& range, const StreetStrength& strength, Street street,
                              double size_pots, double pot_odds, const ArchetypeParams& params);
// A raise of a bet of `size_pots`: the size sets the raise share cap.
WeightedRange raise_likelihood(const WeightedRange& range, const StreetStrength& strength, double size_pots,
                               const ArchetypeParams& params);

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

// `range` facing a bet of `size_pots` at `pot_odds`, on the street `strength` is for.
Response predict_postflop_response(const WeightedRange& range, const StreetStrength& strength, Street street,
                                   double size_pots, double pot_odds, const ArchetypeParams& params);

// `villain_range_so_far` facing a preflop raise from `raiser`, which makes
// `raises_after_hero` raises in all (1 = an open, 2 = a 3-bet, 3+ = a 4-bet or
// more). The chart and widths are the inference's for that spot (see above), with
// the call width also multiplied by (1 / size_ratio)^0.5, size_ratio being the
// raise's size over the standard one: bigger raises lose their weakest calls
// first, smaller ones gain calls. The raises are unchanged, so the continue share
// moves by somewhat less than that factor. The stack-size overrides are not
// applied here.
Response predict_preflop_response(const WeightedRange& villain_range_so_far, Position villain, Position raiser,
                                  int raises_after_hero, double size_ratio, int players,
                                  const ArchetypeParams& params);

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
