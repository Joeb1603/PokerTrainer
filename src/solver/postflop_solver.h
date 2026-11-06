#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <vector>

#include "range/weighted_range.h"
#include "solver/betting_tree.h"
#include "solver/dcfr.h"
#include "solver/showdown.h"

namespace poker {

// A heads-up postflop spot: the public betting state plus each player's range
// (player 0 = OOP, 1 = IP). A player's hands are the combos with weight > 0 that
// hold no board card.
struct SolveSpot : BettingRoot {
    std::array<WeightedRange, 2> ranges;
};

struct SolverLimits {
    std::size_t memory_bytes = std::size_t{2} << 30;
    // Accepted for the API; the river solve runs on the calling thread, which keeps
    // results identical for any value. Stage 8 splits chance subtrees over a pool.
    int threads = 1;
};

// Thrown by the constructor, before any large allocation, when a solve would need
// more than SolverLimits::memory_bytes.
class SolverMemoryError : public std::runtime_error {
public:
    SolverMemoryError(std::size_t required, std::size_t limit);
    std::size_t required() const { return required_; }
    std::size_t limit() const { return limit_; }

private:
    std::size_t required_ = 0;
    std::size_t limit_ = 0;
};

// The root decision, for the player to act there.
struct RootSolution {
    int player = 0;
    std::vector<TreeAction> actions;
    std::vector<int> hands;     // Combo indices of the root player's hands
    std::vector<double> share;  // Each hand's probability at the root: range weight x unblocked opponent weight, sums to 1
    std::vector<std::vector<float>> freq;  // [action][hand], the average strategy
    // [action][hand]: the hand's expected chips after taking the action, with both
    // players following the average strategy afterwards. Chips are relative to the
    // root: received from the final pot minus added after the root, so a fold
    // there is 0 and the pot already in the middle counts as winnable.
    //
    // For an action the hand rarely takes, `ev` reads the hand's own average strategy
    // at later nodes it seldom reaches, which early iterations dominate, so it can
    // undervalue the action by most of a big blind. Use `ev_best` to compare actions.
    std::vector<std::vector<float>> ev;
    // [action][hand]: the same, except that after the action the root player plays a
    // best response to the opponent's average strategy (the opponent still plays its
    // average). So ev_best >= ev, nearly equal for actions the hand takes often once
    // converged, and the largest ev_best of a hand is its best-response value: the
    // share-weighted sum of those maxima is best_response_values()[player]. The
    // regret of choosing action a with hand h is max_b ev_best[b][h] - ev_best[a][h].
    std::vector<std::vector<float>> ev_best;
    std::vector<float> hand_ev;  // [hand]: sum over actions of freq x ev
};

// Discounted CFR over the whole range-vs-range tree.
//
// Utilities: u_p = chips p receives from the pot at the end minus chips p adds after
// the root. Folding has value -(chips the folder added), and the winner gets the
// pot at the root plus what the folder added. Every outcome splits the root pot, so
// u_0 + u_1 = root pot: a constant-sum game, where exploitability is
// (BR_0(avg_1) + BR_1(avg_0) - root pot) / 2.
//
// Each iteration t updates player 0 against player 1's current strategy, then
// player 1 against player 0's updated one (alternating updates). Regrets and
// strategy sums are float per (decision node, action, hand); sums inside a pass run
// in double in a fixed order, so a solve repeats bit for bit. Chance nodes weight
// each card uniformly over the cards neither player's hand blocks.
//
// Not safe for concurrent use: even the const queries share one scratch buffer.
class PostflopSolver {
public:
    // Throws std::invalid_argument for an invalid spot (see build_betting_tree), a
    // player with no hands, negative threads or iterations; SolverMemoryError over
    // the memory limit.
    PostflopSolver(SolveSpot spot, BetAbstraction abstraction, SolverLimits limits = {}, DcfrParams params = {});

    PostflopSolver(const PostflopSolver&) = delete;
    PostflopSolver& operator=(const PostflopSolver&) = delete;

    // Bytes a solve of this spot holds: the tree, hands, showdown tables, regrets,
    // strategy sums and scratch. Builds the tree (small) but allocates nothing else.
    static std::size_t estimate_bytes(const SolveSpot& spot, const BetAbstraction& abstraction);

    // Runs up to `iterations` more iterations, checking `stop` before each one.
    void run(int iterations, std::stop_token stop = {});
    int iterations() const { return iterations_; }

    // Best-response values of each player against the other's average strategy,
    // in chips per hand dealt (relative to the root, as above).
    std::array<double, 2> best_response_values() const;
    // (BR_0 + BR_1 - root pot) / 2 as a percentage of the root pot.
    double exploitability_pct_pot() const;

    RootSolution root() const;

    const BettingTree& tree() const { return tree_; }
    std::span<const int> hands(int player) const { return hands_.combos(player); }
    // A decision node's average strategy, [action][hand] over the acting player's hands.
    std::vector<float> average_strategy(int node) const;
    // Heap bytes actually held, from capacities: estimate_bytes bounds this.
    std::size_t allocated_bytes() const;

private:
    BettingTree tree_;
    MatchupHands hands_;
    std::array<std::vector<double>, 2> weights_;
    std::vector<ShowdownTable> showdowns_;
    std::vector<int> showdown_of_board_;  // Index into showdowns_, or -1 for a partial board
    std::vector<std::size_t> offset_;     // Decision nodes: the first float of the node's block
    std::vector<float> regrets_;
    std::vector<float> strategy_sum_;
    mutable std::vector<double> scratch_;
    SolverLimits limits_;
    DcfrParams params_;
    int iterations_ = 0;
};

}  // namespace poker
