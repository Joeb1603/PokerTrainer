#include "solver/postflop_solver.h"

#include <algorithm>
#include <string>
#include <utility>

#include "range/combos.h"

namespace poker {
namespace {

template <typename T>
std::size_t capacity_bytes(const std::vector<T>& values) {
    return values.capacity() * sizeof(T);
}

std::vector<int> hands_in(const WeightedRange& range, std::span<const Card> board) {
    const CardMask dead = card_mask(board);
    std::vector<int> combos;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (range.weight(combo) > 0.0f && (combo_mask(combo) & dead) == 0) combos.push_back(combo);
    }
    return combos;
}

// Where everything goes, worked out from the tree and the hand counts alone so the
// memory estimate needs no allocation of its own.
struct StoragePlan {
    std::vector<std::size_t> offset;  // Per node; decision nodes only
    std::size_t floats = 0;           // Regret (and strategy sum) entries
    std::size_t scratch = 0;          // Doubles of traversal scratch
    std::size_t full_boards = 0;
};

// Scratch a walk below a node takes, at most, per hand of the larger list:
// a decision keeps its strategy, every child's values and one child reach
// ((2A + 1) lists); a chance node keeps two reaches and one child's values.
StoragePlan plan_storage(const BettingTree& tree, int hands0, int hands1) {
    const std::array<std::size_t, 2> n{static_cast<std::size_t>(hands0), static_cast<std::size_t>(hands1)};
    const std::size_t widest = std::max(n[0], n[1]);
    StoragePlan plan;
    plan.offset.assign(tree.nodes.size(), 0);
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const TreeNode& node = tree.nodes[i];
        if (node.kind != NodeKind::Decision) continue;
        plan.offset[i] = plan.floats;
        plan.floats += static_cast<std::size_t>(node.child_count) * n[node.player];
    }
    std::vector<std::size_t> need(tree.nodes.size(), 0);
    for (std::size_t i = tree.nodes.size(); i-- > 0;) {
        const TreeNode& node = tree.nodes[i];
        std::size_t deepest = 0;
        for (const int child : tree.children_of(static_cast<int>(i))) {
            deepest = std::max(deepest, need[static_cast<std::size_t>(child)]);
        }
        const auto actions = static_cast<std::size_t>(node.child_count);
        if (node.kind == NodeKind::Decision) need[i] = (2 * actions + 1) * widest + deepest;
        if (node.kind == NodeKind::Chance) need[i] = 3 * widest + deepest;
    }
    // Ahead of the root's walk: its values, the opponent weight each hand can face,
    // and one root child's values (for root()).
    plan.scratch = 3 * widest + need.front();
    for (const BoardCards& board : tree.boards) plan.full_boards += board.size == 5 ? 1 : 0;
    return plan;
}

std::size_t planned_bytes(const BettingTree& tree, const StoragePlan& plan, int hands0, int hands1) {
    const auto hands = static_cast<std::size_t>(hands0 + hands1);
    return tree.bytes() + MatchupHands::bytes_for(hands0, hands1) + hands * sizeof(double) +
           plan.full_boards * (sizeof(ShowdownTable) + ShowdownTable::bytes_for(hands0, hands1)) +
           tree.boards.size() * sizeof(int) + tree.nodes.size() * sizeof(std::size_t) +
           2 * plan.floats * sizeof(float) + plan.scratch * sizeof(double);
}

enum class Pass : std::uint8_t {
    Train,         // Current strategies; updates the traverser's regrets and strategy sums
    Average,       // Both players on their average strategy
    BestResponse,  // The traverser maximises against the other's average strategy
};

// One traversal for one player (the traverser) over the whole tree. `own` is the
// traverser's reach and `opp` the opponent's, by hand; `out` receives the
// traverser's counterfactual values. Every node is visited on every pass, so each
// decision node of the traverser is discounted exactly once per iteration.
class Walker {
public:
    const BettingTree* tree = nullptr;
    const MatchupHands* hands = nullptr;
    const std::vector<ShowdownTable>* showdowns = nullptr;
    const std::vector<int>* showdown_of_board = nullptr;
    const std::vector<std::size_t>* offset = nullptr;
    const float* strategy_source = nullptr;  // Train: regrets; otherwise strategy sums
    float* regrets = nullptr;                // Train only
    float* strategy_sum = nullptr;           // Train only
    DcfrDiscount discount;
    Pass pass = Pass::Average;
    int traverser = 0;

    void walk(int index, const double* own, const double* opp, double* out, double* scratch) const {
        const TreeNode& node = tree->nodes[static_cast<std::size_t>(index)];
        const int p = traverser;
        const auto np = static_cast<std::size_t>(hands->size(p));
        const auto no = static_cast<std::size_t>(hands->size(1 - p));
        switch (node.kind) {
            case NodeKind::Fold: {
                const double invested = node.invested[static_cast<std::size_t>(p)];
                const double payoff = node.player == p ? -invested : node.pot - invested;
                fold_values(*hands, p, {opp, no}, payoff, {out, np});
                return;
            }
            case NodeKind::Showdown: {
                const int table = (*showdown_of_board)[static_cast<std::size_t>(node.board)];
                const double invested = node.invested[static_cast<std::size_t>(p)];
                const double pot = node.pot;
                (*showdowns)[static_cast<std::size_t>(table)].values(*hands, p, {opp, no}, pot - invested,
                                                                     0.5 * pot - invested, -invested, {out, np});
                return;
            }
            case NodeKind::Chance: chance(index, node, own, opp, out, scratch, np, no); return;
            case NodeKind::Decision:
                if (node.player == p) {
                    own_decision(index, node, own, opp, out, scratch, np);
                } else {
                    opponent_decision(index, node, own, opp, out, scratch, np, no);
                }
                return;
        }
    }

private:
    void strategy(int index, const TreeNode& node, std::size_t hands_count, double* sigma) const {
        const auto actions = static_cast<std::size_t>(node.child_count);
        const float* source = strategy_source + (*offset)[static_cast<std::size_t>(index)];
        regret_match(std::span<const float>(source, actions * hands_count), node.child_count,
                     static_cast<int>(hands_count), std::span<double>(sigma, actions * hands_count));
    }

    void own_decision(int index, const TreeNode& node, const double* own, const double* opp, double* out,
                      double* scratch, std::size_t np) const {
        const auto actions = static_cast<std::size_t>(node.child_count);
        double* sigma = scratch;
        double* values = sigma + actions * np;
        double* child_own = values + actions * np;
        double* rest = child_own + np;
        if (pass != Pass::BestResponse) strategy(index, node, np, sigma);

        const std::span<const int> children = tree->children_of(index);
        for (std::size_t a = 0; a < actions; ++a) {
            const double* reach = own;
            if (pass == Pass::Train) {
                for (std::size_t h = 0; h < np; ++h) child_own[h] = own[h] * sigma[a * np + h];
                reach = child_own;
            }
            walk(children[a], reach, opp, values + a * np, rest);
        }

        if (pass == Pass::BestResponse) {
            for (std::size_t h = 0; h < np; ++h) out[h] = values[h];
            for (std::size_t a = 1; a < actions; ++a) {
                for (std::size_t h = 0; h < np; ++h) out[h] = std::max(out[h], values[a * np + h]);
            }
            return;
        }
        for (std::size_t h = 0; h < np; ++h) out[h] = sigma[h] * values[h];
        for (std::size_t a = 1; a < actions; ++a) {
            for (std::size_t h = 0; h < np; ++h) out[h] += sigma[a * np + h] * values[a * np + h];
        }
        if (pass != Pass::Train) return;

        const std::size_t base = (*offset)[static_cast<std::size_t>(index)];
        float* node_regrets = regrets + base;
        float* node_sums = strategy_sum + base;
        for (std::size_t a = 0; a < actions; ++a) {
            for (std::size_t h = 0; h < np; ++h) {
                const std::size_t i = a * np + h;
                const double regret = values[i] - out[h];
                node_regrets[i] = static_cast<float>(discounted_regret(node_regrets[i], discount) + regret);
                node_sums[i] = static_cast<float>(node_sums[i] * discount.strategy + own[h] * sigma[i]);
            }
        }
    }

    void opponent_decision(int index, const TreeNode& node, const double* own, const double* opp, double* out,
                           double* scratch, std::size_t np, std::size_t no) const {
        const auto actions = static_cast<std::size_t>(node.child_count);
        double* sigma = scratch;
        double* child_opp = sigma + actions * no;
        double* child_values = child_opp + no;
        double* rest = child_values + np;
        strategy(index, node, no, sigma);

        std::fill(out, out + np, 0.0);
        const std::span<const int> children = tree->children_of(index);
        for (std::size_t a = 0; a < actions; ++a) {
            for (std::size_t o = 0; o < no; ++o) child_opp[o] = opp[o] * sigma[a * no + o];
            walk(children[a], own, child_opp, child_values, rest);
            for (std::size_t h = 0; h < np; ++h) out[h] += child_values[h];
        }
    }

    // Each card is equally likely among those neither hand holds: with both hands
    // and the board known, that is the children less the four hole cards.
    void chance(int index, const TreeNode& node, const double* own, const double* opp, double* out,
                double* scratch, std::size_t np, std::size_t no) const {
        const int p = traverser;
        double* child_own = scratch;
        double* child_opp = child_own + np;
        double* child_values = child_opp + no;
        double* rest = child_values + np;

        std::fill(out, out + np, 0.0);
        const std::span<const int> children = tree->children_of(index);
        for (std::size_t k = 0; k < children.size(); ++k) {
            const int card = tree->dealt[static_cast<std::size_t>(node.first_child) + k].id;
            std::copy(own, own + np, child_own);
            std::copy(opp, opp + no, child_opp);
            for (const int h : hands->with_card(p, card)) child_own[h] = 0.0;
            for (const int o : hands->with_card(1 - p, card)) child_opp[o] = 0.0;
            walk(children[k], child_own, child_opp, child_values, rest);
            for (const int h : hands->with_card(p, card)) child_values[h] = 0.0;
            for (std::size_t h = 0; h < np; ++h) out[h] += child_values[h];
        }
        const double scale = 1.0 / static_cast<double>(children.size() - 4);
        for (std::size_t h = 0; h < np; ++h) out[h] *= scale;
    }
};

}  // namespace

SolverMemoryError::SolverMemoryError(std::size_t required, std::size_t limit)
    : std::runtime_error("solver: needs " + std::to_string(required) + " bytes, over the limit of " +
                         std::to_string(limit)),
      required_(required),
      limit_(limit) {}

std::size_t PostflopSolver::estimate_bytes(const SolveSpot& spot, const BetAbstraction& abstraction) {
    const BettingTree tree = build_betting_tree(spot, abstraction);
    const int hands0 = static_cast<int>(hands_in(spot.ranges[0], spot.board).size());
    const int hands1 = static_cast<int>(hands_in(spot.ranges[1], spot.board).size());
    return planned_bytes(tree, plan_storage(tree, hands0, hands1), hands0, hands1);
}

PostflopSolver::PostflopSolver(SolveSpot spot, BetAbstraction abstraction, SolverLimits limits, DcfrParams params)
    : limits_(limits), params_(params) {
    if (limits.threads < 0) throw std::invalid_argument("solver: threads must not be negative");
    tree_ = build_betting_tree(spot, abstraction);
    const std::array<std::vector<int>, 2> combos{hands_in(spot.ranges[0], spot.board),
                                                 hands_in(spot.ranges[1], spot.board)};
    if (combos[0].empty() || combos[1].empty()) throw std::invalid_argument("solver: a range has no hands");
    const int hands0 = static_cast<int>(combos[0].size());
    const int hands1 = static_cast<int>(combos[1].size());

    StoragePlan plan = plan_storage(tree_, hands0, hands1);
    const std::size_t required = planned_bytes(tree_, plan, hands0, hands1);
    if (required > limits.memory_bytes) throw SolverMemoryError(required, limits.memory_bytes);

    hands_ = MatchupHands(combos[0], combos[1]);
    for (std::size_t p = 0; p < 2; ++p) {
        weights_[p].reserve(combos[p].size());
        for (const int combo : combos[p]) weights_[p].push_back(spot.ranges[p].weight(combo));
    }
    showdowns_.reserve(plan.full_boards);
    showdown_of_board_.assign(tree_.boards.size(), -1);
    for (std::size_t b = 0; b < tree_.boards.size(); ++b) {
        if (tree_.boards[b].size != 5) continue;
        showdown_of_board_[b] = static_cast<int>(showdowns_.size());
        showdowns_.emplace_back(hands_, tree_.boards[b].view());
    }
    offset_ = std::move(plan.offset);
    regrets_.assign(plan.floats, 0.0f);
    strategy_sum_.assign(plan.floats, 0.0f);
    scratch_.assign(plan.scratch, 0.0);

    // Some pair of hands must be dealable together, or nothing has a value.
    std::span<double> faced(scratch_.data(), static_cast<std::size_t>(hands0));
    fold_values(hands_, 0, weights_[1], 1.0, faced);
    double mass = 0.0;
    for (std::size_t h = 0; h < faced.size(); ++h) mass += weights_[0][h] * faced[h];
    if (!(mass > 0.0)) throw std::invalid_argument("solver: the ranges cannot be dealt together");
}

void PostflopSolver::run(int iterations, std::stop_token stop) {
    if (iterations < 0) throw std::invalid_argument("solver: iterations must not be negative");
    const std::size_t widest = static_cast<std::size_t>(std::max(hands_.size(0), hands_.size(1)));
    for (int i = 0; i < iterations; ++i) {
        if (stop.stop_requested()) return;
        ++iterations_;
        Walker walker;
        walker.tree = &tree_;
        walker.hands = &hands_;
        walker.showdowns = &showdowns_;
        walker.showdown_of_board = &showdown_of_board_;
        walker.offset = &offset_;
        walker.strategy_source = regrets_.data();
        walker.regrets = regrets_.data();
        walker.strategy_sum = strategy_sum_.data();
        walker.discount = dcfr_discount(params_, iterations_);
        walker.pass = Pass::Train;
        for (int p = 0; p < 2; ++p) {
            walker.traverser = p;
            const auto me = static_cast<std::size_t>(p);
            walker.walk(0, weights_[me].data(), weights_[1 - me].data(), scratch_.data(), scratch_.data() + 3 * widest);
        }
    }
}

std::array<double, 2> PostflopSolver::best_response_values() const {
    const std::size_t widest = static_cast<std::size_t>(std::max(hands_.size(0), hands_.size(1)));
    Walker walker;
    walker.tree = &tree_;
    walker.hands = &hands_;
    walker.showdowns = &showdowns_;
    walker.showdown_of_board = &showdown_of_board_;
    walker.offset = &offset_;
    walker.strategy_source = strategy_sum_.data();
    walker.pass = Pass::BestResponse;

    std::array<double, 2> result{};
    for (int p = 0; p < 2; ++p) {
        const auto me = static_cast<std::size_t>(p);
        const auto n = weights_[me].size();
        double* values = scratch_.data();
        double* faced = values + widest;
        walker.traverser = p;
        walker.walk(0, weights_[me].data(), weights_[1 - me].data(), values, scratch_.data() + 3 * widest);
        fold_values(hands_, p, weights_[1 - me], 1.0, {faced, n});
        double total = 0.0;
        double mass = 0.0;
        for (std::size_t h = 0; h < n; ++h) {
            total += weights_[me][h] * values[h];
            mass += weights_[me][h] * faced[h];
        }
        result[me] = total / mass;
    }
    return result;
}

double PostflopSolver::exploitability_pct_pot() const {
    const std::array<double, 2> best = best_response_values();
    const double pot = tree_.root_pot;
    return 100.0 * (best[0] + best[1] - pot) / 2.0 / pot;
}

std::vector<float> PostflopSolver::average_strategy(int node) const {
    const TreeNode& entry = tree_.nodes.at(static_cast<std::size_t>(node));
    if (entry.kind != NodeKind::Decision) throw std::invalid_argument("solver: not a decision node");
    const auto n = static_cast<std::size_t>(hands_.size(entry.player));
    const std::size_t count = static_cast<std::size_t>(entry.child_count) * n;
    std::vector<float> strategy(count);
    regret_match(std::span<const float>(strategy_sum_.data() + offset_[static_cast<std::size_t>(node)], count),
                 entry.child_count, static_cast<int>(n), std::span<float>(strategy));
    return strategy;
}

RootSolution PostflopSolver::root() const {
    const TreeNode& node = tree_.root();
    const int player = node.player;
    const auto me = static_cast<std::size_t>(player);
    const auto n = weights_[me].size();
    const auto actions = static_cast<std::size_t>(node.child_count);
    const std::size_t widest = static_cast<std::size_t>(std::max(hands_.size(0), hands_.size(1)));

    RootSolution solution;
    solution.player = player;
    const std::span<const TreeAction> root_actions = tree_.actions_of(0);
    solution.actions.assign(root_actions.begin(), root_actions.end());
    const std::span<const int> combos = hands_.combos(player);
    solution.hands.assign(combos.begin(), combos.end());

    double* faced = scratch_.data() + widest;
    double* values = scratch_.data() + 2 * widest;
    fold_values(hands_, player, weights_[1 - me], 1.0, {faced, n});
    double mass = 0.0;
    for (std::size_t h = 0; h < n; ++h) mass += weights_[me][h] * faced[h];
    solution.share.resize(n);
    for (std::size_t h = 0; h < n; ++h) solution.share[h] = weights_[me][h] * faced[h] / mass;

    const std::vector<float> average = average_strategy(0);
    solution.freq.assign(actions, std::vector<float>(n));
    solution.ev.assign(actions, std::vector<float>(n));
    solution.ev_best.assign(actions, std::vector<float>(n));
    solution.hand_ev.assign(n, 0.0f);

    Walker walker;
    walker.tree = &tree_;
    walker.hands = &hands_;
    walker.showdowns = &showdowns_;
    walker.showdown_of_board = &showdown_of_board_;
    walker.offset = &offset_;
    walker.strategy_source = strategy_sum_.data();
    walker.pass = Pass::Average;
    walker.traverser = player;
    std::vector<double> hand_ev(n, 0.0);
    for (std::size_t a = 0; a < actions; ++a) {
        walker.walk(tree_.child(0, static_cast<int>(a)), weights_[me].data(), weights_[1 - me].data(), values,
                    scratch_.data() + 3 * widest);
        for (std::size_t h = 0; h < n; ++h) {
            const double ev = faced[h] > 0.0 ? values[h] / faced[h] : 0.0;
            const float freq = average[a * n + h];
            solution.freq[a][h] = freq;
            solution.ev[a][h] = static_cast<float>(ev);
            hand_ev[h] += freq * ev;
        }
    }
    for (std::size_t h = 0; h < n; ++h) solution.hand_ev[h] = static_cast<float>(hand_ev[h]);

    // The same children again with the root player best-responding below the root.
    walker.pass = Pass::BestResponse;
    for (std::size_t a = 0; a < actions; ++a) {
        walker.walk(tree_.child(0, static_cast<int>(a)), weights_[me].data(), weights_[1 - me].data(), values,
                    scratch_.data() + 3 * widest);
        for (std::size_t h = 0; h < n; ++h) {
            solution.ev_best[a][h] = static_cast<float>(faced[h] > 0.0 ? values[h] / faced[h] : 0.0);
        }
    }
    return solution;
}

std::size_t PostflopSolver::allocated_bytes() const {
    std::size_t total = tree_.bytes() + hands_.bytes() + capacity_bytes(weights_[0]) + capacity_bytes(weights_[1]) +
                        capacity_bytes(showdowns_) + capacity_bytes(showdown_of_board_) + capacity_bytes(offset_) +
                        capacity_bytes(regrets_) + capacity_bytes(strategy_sum_) + capacity_bytes(scratch_);
    for (const ShowdownTable& table : showdowns_) total += table.bytes();
    return total;
}

}  // namespace poker
