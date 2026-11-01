#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "engine/table_types.h"
#include "engine/player_view.h"
#include "equity/equity.h"

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

struct HintRecommendation {
    std::string best_action;  // e.g. "RAISE to 150 (75% Pot)"
    Action action;            // The same advice, ready for apply_action
    double equity_pct = 0.0;
    double pot_odds_pct = 0.0;  // 0 when there is nothing to call
    double mdf_pct = 100.0;     // 100 when not facing a bet
    double spr = 0.0;
    EvEstimates ev;       // With the size-fit adjustment, so the advice is the highest listed
    EvEstimates base_ev;  // The model alone, which is what mistakes are judged on
    std::optional<int> raise_25_to;
    std::optional<int> raise_50_to;
    std::optional<int> raise_75_to;
    std::optional<int> raise_100_to;
    std::string rationale;
};

// Coaching for one decision, computed from the player's sanitized view alone.
//
// The EVs are heuristic estimates from an assumed opponent model, not solver
// output. The model, for pot P, equity e and a raise that adds a chips and can be
// matched with m:
//
//   Fold        0
//   Check       e_p * P
//   Call        (e_p - pressure) * (P + call) - call
//   Bet/raise   f * P * k + (1 - f) * (e_c * (P + a + m) - a + implied)
//
// e_p is equity weighted over the pot's layers, each against the opponents who can
// win it, so a short all-in only contests its own part (see pot_layers), and P in
// the check and call lines counts only chips a call can win. pressure is the
// bet_pressure discount the GTO bot uses: random-hand equity flatters a hand against
// a real betting range.
//
// Fold equity f assumes each opponent still able to fold calls at the Minimum
// Defense Frequency for a bet of s = a / P pots, with a floor because some hands
// never fold: 10% when nobody has bet, and min(60%, 10% + 90% / s) when raising a
// bet, as a player who has already put money in folds less. All of them must fold,
// so f = (1 - call)^n. k is the equity left in the pot after that: 1, unless a
// player already all in still contests part of it.
//
// e_c, the equity when called, is lower than e because callers are not random
// hands. With n > 1 it starts from the average of e and heads-up equity, since one
// caller is likelier than all of them, then becomes base^(1 + f + o), or
// base^(2 + f + o) when raising a bet, where o = 0.25 * (s - 1) for an overbet, as
// the hands that call one are stronger still. `implied` rewards a raise that keeps chips behind for later
// streets, which a single-street formula otherwise misses and which would make
// all-in look best with every strong hand:
// (e_c - 0.5, if positive) * effective stack left * 0.5, before the river only, and at
// most half the pot once called. Without that cap the stack behind outweighs everything
// else, and the nuts would choose the smallest bet.
//
// Postflop, that model decides whether to bet at all, comparing the best bet with
// checking, calling and folding. It says little about which size, so a rule chooses:
//
//   100%  A very strong hand on the river, or on a wet and dynamic board
//   25%   Any other hand on a monotone board; thin value on a dry or paired one
//   50%   Other value on a dry or paired board; thin value on a wet one
//   75%   Solid value or a strong draw on a wet board
//
// The chosen size is listed at the best bet's EV, its fit adjustment, so `ev` shows the
// advice as the highest option while `base_ev` keeps the model's own numbers. Fit never
// turns a check, call or fold into a bet. All-in is chosen only when it has the best
// model EV and costs at most 1.5 pots, or is the only raise left.
//
// Unopened preflop pots replace that fold model: nobody has shown interest, so each
// opponent calls about 15% of the time at a standard open, proportionally less
// against bigger sizes (1% at least), and the base equity is heads-up since a caller
// is usually alone. A limper has shown interest, so a limped pot keeps the usual model.
//
// Preflop, random-hand equity also knows nothing about position, so the 6-max charts
// act as a prior. Options the chart rejects lose a big blind plus a quarter of the
// chips they risk: the bigger the play, the more the range it faces matters. A call
// the chart picks gains the same, and a raise it picks gains just the big blind.
// Strong EV differences can still outvote the chart.
//
// None of those constants come from a solved game: expect sensible direction, not
// exact numbers. The model ignores future betting apart from `implied` and position
// when calling, and a called raise is valued as one pot rather than by layers.
class TrainingAdvisor {
public:
    explicit TrainingAdvisor(std::uint64_t seed = 0, int rollouts = kDefaultRollouts)
        : seed_(seed), rollouts_(rollouts) {}

    // The result depends only on the seed and the view, so a GUI can call this every
    // frame and the hint will not flicker. Throws std::invalid_argument unless the
    // view's seat holds cards and is the one to act.
    HintRecommendation analyze(const PlayerView& view) const;

private:
    std::uint64_t seed_;
    int rollouts_;
};

}  // namespace poker
