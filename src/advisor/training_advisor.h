#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "engine/table_types.h"
#include "engine/player_view.h"
#include "equity/equity.h"
#include "range/range_model.h"

namespace poker {

// Estimated chips won or lost from this decision on, relative to folding now.
// An absent option is not legal here, or would duplicate another: a 75% raise that
// takes the whole stack is listed only as all-in, and sizes that clamp to the same
// bet are listed once, under the smallest.
struct EvEstimates {
    std::optional<double> fold;
    std::optional<double> check_call;
    std::optional<double> raise_25;
    std::optional<double> raise_50;
    std::optional<double> raise_75;
    std::optional<double> raise_100;
    std::optional<double> all_in;
};

// How the opponents are predicted to answer one bet or raise, in percent (0-100).
struct BetOutcome {
    double fold_pct = 0.0;           // Everyone still able to fold does
    double raise_pct = 0.0;          // At least one of them raises
    double equity_called_pct = 0.0;  // Hero's equity against the hands that call
    double equity_raised_pct = 0.0;  // ... and against the hands that raise
};

// Index of each bet size in the `outcomes` arrays, matching the EvEstimates rows.
inline constexpr std::size_t kOutcome25 = 0;
inline constexpr std::size_t kOutcome50 = 1;
inline constexpr std::size_t kOutcome75 = 2;
inline constexpr std::size_t kOutcome100 = 3;
inline constexpr std::size_t kOutcomeAllIn = 4;
inline constexpr std::size_t kOutcomeCount = 5;

// The advice against one player type (Nit, Station or Maniac): the same model run
// with that type's ranges and parameters (see "Archetype lines" below).
struct AdviceLine {
    Archetype versus = Archetype::Nit;
    std::string best_action;
    Action action;
    EvEstimates ev;  // With the size-fit adjustment; there is no base_ev per line
    double range_equity_pct = 0.0;
    std::array<std::optional<BetOutcome>, kOutcomeCount> outcomes;  // Rows as the hint's
    std::string why;  // One sentence, at most 140 characters, on what differs from Baseline
};

struct HintRecommendation {
    std::string best_action;  // e.g. "RAISE to 150 (75% Pot)"
    Action action;            // The same advice, ready for apply_action
    double equity_pct = 0.0;        // Against random hands, by Monte Carlo (spec section 5, the log's `eq`)
    double range_equity_pct = 0.0;  // Against the estimated ranges of every opponent still in
    double pot_odds_pct = 0.0;  // 0 when there is nothing to call
    double mdf_pct = 100.0;     // 100 when not facing a bet
    double spr = 0.0;
    EvEstimates ev;       // With the size-fit adjustment, so the advice is the highest listed
    EvEstimates base_ev;  // The model alone, which is what mistakes are judged on
    std::optional<int> raise_25_to;
    std::optional<int> raise_50_to;
    std::optional<int> raise_75_to;
    std::optional<int> raise_100_to;
    // The predicted answers behind each listed raise row (25/50/75/100%, all-in).
    // The all-in entry is absent when the all-in is only a call.
    std::array<std::optional<BetOutcome>, kOutcomeCount> outcomes;
    // Against Nit, Station and Maniac, in that order; empty when
    // AdvisorOptions::archetype_lines is off. The fields above are Baseline's.
    std::array<std::optional<AdviceLine>, 3> versus;
    std::string rationale;
};

struct AdvisorOptions {
    int rollouts = kDefaultRollouts;  // Random-hand Monte Carlo, for equity_pct
    int range_rollouts = 3000;        // Multiway range Monte Carlo (equity_vs_ranges)
    // Flop range equity: river cards per turn card; 0 (or 23 and above) is exact.
    int flop_rivers_per_turn = 0;
    int threads = 0;  // 0 = automatic; otherwise at most this many
    bool archetype_lines = true;  // Fill HintRecommendation::versus
};

// Coaching for one decision, computed from the player's sanitized view alone.
//
// The EVs are heuristic estimates from an assumed opponent model (range_model.h,
// Baseline type), not solver output: expect sensible direction, not exact numbers.
//
// --- Equity -------------------------------------------------------------------
//
// equity_pct is Hero's equity against random hands by Monte Carlo, as the spec asks;
// it feeds the hand log and the bet-size rule below, and nothing else. The EVs use
// range equity instead. Each opponent still in the hand has a range from
// infer_ranges(view, Baseline), and Hero's own range as the others see it is
// infer_range_for(view, Hero, Baseline) (board removed, Hero's cards kept). E is
// Hero's equity against every combo of those ranges (hero_vs_each_combo: exact on
// the turn and river, exact on the flop by default, and the 169-class preflop
// matrix before the flop), and e(R) is E averaged over a range R.
//
// For a set of opponents S: e(R_i) when S is one opponent, a Monte Carlo over all
// the ranges (equity_vs_ranges, range_rollouts) when it is several, and 1 when it is
// empty. If the ranges have narrowed so far that they can almost never be dealt
// together, the opponents are treated as independent: the product of the heads-up
// equities. range_equity_pct is that against every opponent still in. The layered
// equity e_L weights each pot layer (pot_layers) by its chips, each against its own
// opponents, so a short all-in contests only its part.
//
// --- EV of each option, for pot P (view.total_pot) -----------------------------
//
//   Fold   0
//   Check  e_L * contestable
//   Call   e_L * (contestable + to_call) - to_call
//   Bet/raise adding a chips (to the street total clamped to what an opponent can
//   match), each opponent i matching it with m_i chips:
//          F * P * k + P_call * V_called + P_raise * V_raised
//
// Each opponent i still able to fold answers with predict_postflop_response, or
// predict_preflop_response before the flop (below): fold_i, call_i, raise_i and the
// call and raise ranges. Then
//   F       = product of fold_i: everybody folds
//   P_raise = 1 - product of (1 - raise_i): somebody raises
//   P_call  = 1 - F - P_raise: somebody calls and nobody raises
// One caller is assumed, the opponents weighted by call_i (w_i) or raise_i (u_i),
// each set of weights scaled to sum to 1:
//   e_call  = sum w_i e(call range_i), m = sum w_i m_i
//   V_called = e_call * (P + a + m) - a + implied
//   V_raised = sum u_i max(-a, e(raise range_i) * (P + T_i + T'_i) - T_i)
// The raise is taken to be pot-sized, to R2_i = to + (P + a + m_i), capped at both
// stacks (Hero's street bet plus stack, and the raiser's) and at least `to`; Hero
// calls it at a cost of T_i = R2_i - Hero's street bet, the raiser having put in
// T'_i = R2_i - its street bet from now on, or folds for -a, whichever is better.
// When Hero's raise is all in, R2_i = to and V_raised is a call. k is the share of
// the pot Hero keeps once everybody foldable has folded: 1, unless a player already
// all in contests part of it, when each layer counts at e against its all-in players.
//
// implied rewards a raise that keeps chips behind for later streets, which a
// single-street formula otherwise misses and which would make all-in look best with
// every strong hand: (e_call - 0.5, if positive) * effective stack left * 0.5,
// before the river only, and at most 0.5 * (P + a + m). Without that cap the stack
// behind outweighs everything else, and the nuts would choose the smallest bet.
//
// Postflop answers: size s = (to - current bet) / (P + Hero's call), the pot
// fraction raise_to_for_pot_fraction uses; pot odds m_i / (P + a + m_i); and Hero's
// range H as the bettor's range, as it stands before this bet or raise. (Narrowing
// H by the bet first was tried and measured slightly worse on the river bench.)
//
// Preflop answers: predict_preflop_response for every opponent still able to act,
// with its range so far, its position, Hero's position as the raiser, the raise
// levels including Hero's (a short all-in is not a new level, as the engine rules),
// the players dealt in, and villain_limped for a player who called before any
// raise. size_ratio is the raise's street total over a standard one: 2.5 big
// blinds plus one per caller first in, 3x the bet plus one bet per caller against
// one raise, 2.3x against two, and all in after that, as the chart bots size. The
// call ranges are scored on the preflop matrix, all in, so postflop play and
// position are left out; the chart prior below stands in for them.
//
// --- Bet size -----------------------------------------------------------------
//
// Postflop, the model decides whether to bet at all, comparing the best bet with
// checking, calling and folding. It says little about which size, so a rule
// chooses, using random-hand equity (its bars were set on it):
//
//   100%  A very strong hand on the river, or on a wet and dynamic board
//   25%   Any other hand on a monotone board; thin value on a dry or paired one
//   50%   Other value on a dry or paired board; thin value on a wet one
//   75%   Solid value or a strong draw on a wet board
//
// The chosen size is listed at the best bet's EV, its fit adjustment, so `ev` shows
// the advice as the highest option while `base_ev` keeps the model's own numbers.
// Fit never turns a check, call or fold into a bet. All-in is chosen only when it
// has the best model EV and costs at most 1.5 pots, or is the only raise left.
//
// --- Preflop chart prior --------------------------------------------------------
//
// Preflop, the 6-max charts act as a prior (chart_response, which also turns a
// fold into a call at a tiny price). Options the chart rejects lose a big blind
// plus a quarter of the chips they risk: the bigger the play, the more the
// range it faces matters. A call the chart picks gains the same, and a raise it
// picks gains just the big blind. Strong EV differences can still outvote it.
// Preflop chooses among the ladder sizes by EV; there is no size rule. All-in is
// advised preflop only with an effective stack of at most 15 big blinds, within
// 1.5 pots, or as the only raise left: the single-street model overrates deep
// shoves with strong hands, as their callers are the top of each range while a
// normal raise keeps the rest in for later streets. Its `ev` is then capped at the
// advice, as postflop; `base_ev` keeps the model's number.
//
// --- Archetype lines ------------------------------------------------------------
//
// With archetype_lines, each of Nit, Station and Maniac gets the whole pipeline
// above again, with infer_ranges(view, type) for every opponent and that type's
// archetype_params for their answers (one type for all opponents). Shared with
// Baseline, so the lines differ only by the opponents' type:
//   - the per-combo vector E, computed once over the union of every line's
//     ranges (each entry depends only on its own combo, so Baseline's numbers are
//     bit-identical with the lines on or off). The multiway Monte Carlo runs per
//     line with Baseline's seed; postflop with a third of range_rollouts, and
//     fewer where a 512-deal trial finds most deals of the narrowed ranges clash
//     (about 150,000 deals in all, at least 100 rollouts), or none, when no trial
//     deal fits: then the independent-opponents product above. Line equities
//     multiway are therefore noisier than Baseline's, by about 1-2 pp.
//   - Hero's range as the bettor's range: as Baseline opponents read it. The
//     types differ in how they answer a bet, not in how they read Hero, and a
//     fixed bettor range keeps each line's difference to the answers alone.
//   - the full chart prior preflop. A weaker prior was measured and rejected: at
//     half weight the lines 3-bet 152 (Station) and 154 (Maniac) of 169 classes
//     on the button against a cutoff open, though only 2 and 0 of them when
//     Baseline uses the same half weight, so the change came from the weaker
//     prior, not from the type (docs/advisor-upgrade-notes.md, Stage 5 batch B).
// Postflop, a value bet moves value_size_shift rungs along the ladder (Nit -1,
// Station +1, Maniac 0), within its ends. A value bet is one the size rule calls
// solid or very strong; probes, thin value and draws keep their size. The move
// happens only where betting already beats the passive options, so it never
// turns a check, call or fold into a bet, and it lands on all-in only where
// all-in may be chosen anyway (within 1.5 pots). Preflop has no shift: the EV
// chooses the size there. `why` is generated from the numbers: the fold share,
// equity when called, or range equity against pot odds, set against Baseline's.
// The lines run on threads of their own unless options.threads is 1; the result
// is the same either way. Mistakes and the log use Baseline only.
//
// --- Limits ---------------------------------------------------------------------
//
// None of the constants here or in the range model come from a solved game. The
// model ignores future betting apart from `implied`, position when calling, and a
// second caller; a called raise is one pot rather than layers.
class TrainingAdvisor {
public:
    // Throws std::invalid_argument for non-positive rollouts or range_rollouts, or
    // negative threads or flop_rivers_per_turn.
    TrainingAdvisor(std::uint64_t seed, AdvisorOptions options);

    // `rollouts` random-hand rollouts and 3/5 as many multiway range rollouts (at
    // least 100), so the default 5000 gives the default 3000; the rest default.
    explicit TrainingAdvisor(std::uint64_t seed = 0, int rollouts = kDefaultRollouts);

    // The result depends only on the seed and the view, so a GUI can call this every
    // frame and the hint will not flicker. Throws std::invalid_argument unless the
    // view's seat holds cards and is the one to act.
    HintRecommendation analyze(const PlayerView& view) const;

    const AdvisorOptions& options() const { return options_; }

private:
    std::uint64_t seed_;
    AdvisorOptions options_;
};

}  // namespace poker
