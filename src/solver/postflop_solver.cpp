#include "solver/postflop_solver.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "range/combos.h"
#include "solver/worker_pool.h"

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

std::size_t add_sat(std::size_t a, std::size_t b) {
    return a > std::numeric_limits<std::size_t>::max() - b ? std::numeric_limits<std::size_t>::max() : a + b;
}

std::size_t mul_sat(std::size_t a, std::size_t b) {
    if (a == 0 || b == 0) return 0;
    return a > std::numeric_limits<std::size_t>::max() / b ? std::numeric_limits<std::size_t>::max() : a * b;
}

// Where the floats and scratch go, worked out from the tree's shape and the hand
// counts alone, so the memory estimate needs neither the tree nor an allocation.
//
// Scratch, in doubles, one list = one double per hand of the larger range:
// - main: what the calling thread's walk needs: three lists ahead of the root's
//   walk (its values, the opponent weight each hand can face and one root child's
//   values, for root()) plus TreeShape::walk_lists. A decision node holds 2A + 1
//   lists, a chance node 3 (its child reaches and one child's values), plus the
//   deepest child. The calling thread also runs first-layer tasks inside the chance
//   node's own part of this.
// - chance: one list per card of the first chance layer, for the per-card values.
// - workers 1..: each its two child reaches plus the deepest first-layer child.
struct StorageLayout {
    std::size_t floats = 0;  // Regret (and strategy sum) entries
    std::size_t main = 0;
    std::size_t chance = 0;
    std::size_t stride = 0;  // Per extra worker
    int threads = 1;         // After the cap

    std::size_t scratch() const {
        return add_sat(add_sat(main, chance), mul_sat(static_cast<std::size_t>(threads - 1), stride));
    }
};

StorageLayout layout_for(const TreeShape& shape, int hands0, int hands1, int threads) {
    const std::array<std::size_t, 2> n{static_cast<std::size_t>(hands0), static_cast<std::size_t>(hands1)};
    const std::size_t widest = std::max(n[0], n[1]);
    StorageLayout layout;
    layout.floats = add_sat(mul_sat(shape.player_actions[0], n[0]), mul_sat(shape.player_actions[1], n[1]));
    layout.main = mul_sat(add_sat(3, shape.walk_lists), widest);
    if (shape.first_layer_cards > 0) {
        layout.chance = mul_sat(shape.first_layer_cards, widest);
        layout.stride = mul_sat(add_sat(2, shape.first_layer_child_lists), widest);
        const auto cards = static_cast<int>(std::min<std::size_t>(shape.first_layer_cards, 1024));
        layout.threads = std::clamp(threads, 1, cards);
    }
    return layout;
}

std::size_t planned_bytes(const TreeShape& shape, const StorageLayout& layout, int hands0, int hands1) {
    const auto hands = static_cast<std::size_t>(hands0 + hands1);
    std::size_t total = shape.bytes();
    total = add_sat(total, MatchupHands::bytes_for(hands0, hands1) + hands * sizeof(double));
    total = add_sat(total, mul_sat(shape.full_boards, sizeof(ShowdownTable) + ShowdownTable::bytes_for(hands0, hands1)));
    total = add_sat(total, mul_sat(shape.checkdown_boards, sizeof(CheckDownTable) + CheckDownTable::bytes_for(hands0, hands1)));
    total = add_sat(total, mul_sat(shape.boards, 2 * sizeof(int)));
    total = add_sat(total, mul_sat(shape.nodes, sizeof(std::size_t)));
    total = add_sat(total, mul_sat(layout.floats, 2 * sizeof(float)));
    return add_sat(total, mul_sat(layout.scratch(), sizeof(double)));
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
    const std::vector<CheckDownTable>* checkdowns = nullptr;
    const std::vector<int>* checkdown_of_board = nullptr;  // Index into checkdowns, or -1
    const std::vector<std::size_t>* offset = nullptr;
    const float* strategy_source = nullptr;  // Train: regrets; otherwise strategy sums
    float* regrets = nullptr;                // Train only
    float* strategy_sum = nullptr;           // Train only
    DcfrDiscount discount;
    Pass pass = Pass::Average;
    int traverser = 0;
    // The first chance layer: chance nodes dealing this street run one task per card.
    Street parallel_street = Street::Preflop;
    WorkerPool* pool = nullptr;      // Null: the calling thread runs every task
    double* chance_values = nullptr;  // Per card, one list
    double* worker_scratch = nullptr;  // Workers 1..: worker_stride doubles each
    std::size_t worker_stride = 0;

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
            case NodeKind::CheckDown: check_down(node, opp, out, np, no); return;
            case NodeKind::Chance:
                if (node.street == parallel_street) {
                    parallel_chance(index, node, own, opp, out, scratch, np, no);
                } else {
                    chance(index, node, own, opp, out, scratch, np, no);
                }
                return;
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

    // The chance node above, with each card's subtree a task that leaves its values
    // in its own list of chance_values; the lists are then summed in card order, so
    // the result does not depend on which worker ran which card.
    void parallel_chance(int index, const TreeNode& node, const double* own, const double* opp, double* out,
                         double* scratch, std::size_t np, std::size_t no) const {
        const int p = traverser;
        const std::span<const int> children = tree->children_of(index);
        const auto task = [&](int k, int worker) {
            double* child_own = worker == 0 ? scratch : worker_scratch + static_cast<std::size_t>(worker - 1) * worker_stride;
            double* child_opp = child_own + np;
            double* rest = child_opp + no;
            double* values = chance_values + static_cast<std::size_t>(k) * np;
            const auto slot = static_cast<std::size_t>(node.first_child) + static_cast<std::size_t>(k);
            const int card = tree->dealt[slot].id;
            std::copy(own, own + np, child_own);
            std::copy(opp, opp + no, child_opp);
            for (const int h : hands->with_card(p, card)) child_own[h] = 0.0;
            for (const int o : hands->with_card(1 - p, card)) child_opp[o] = 0.0;
            walk(children[static_cast<std::size_t>(k)], child_own, child_opp, values, rest);
            for (const int h : hands->with_card(p, card)) values[h] = 0.0;
        };
        const auto cards = static_cast<int>(children.size());
        if (pool != nullptr) {
            pool->run(cards, task);
        } else {
            for (int k = 0; k < cards; ++k) task(k, 0);
        }
        std::fill(out, out + np, 0.0);
        for (std::size_t k = 0; k < children.size(); ++k) {
            const double* values = chance_values + k * np;
            for (std::size_t h = 0; h < np; ++h) out[h] += values[h];
        }
        const double scale = 1.0 / static_cast<double>(children.size() - 4);
        for (std::size_t h = 0; h < np; ++h) out[h] *= scale;
    }

    // The river checked down, from the turn board's CheckDownTable.
    void check_down(const TreeNode& node, const double* opp, double* out, std::size_t np, std::size_t no) const {
        const double invested = node.invested[static_cast<std::size_t>(traverser)];
        const int table = (*checkdown_of_board)[static_cast<std::size_t>(node.board)];
        (*checkdowns)[static_cast<std::size_t>(table)].values(*hands, traverser, {opp, no}, node.pot, invested, {out, np});
    }
};

}  // namespace

SolverMemoryError::SolverMemoryError(std::size_t required, std::size_t limit)
    : std::runtime_error("solver: needs " + std::to_string(required) + " bytes, over the limit of " +
                         std::to_string(limit)),
      required_(required),
      limit_(limit) {}

std::size_t PostflopSolver::estimate_bytes(const SolveSpot& spot, const BetAbstraction& abstraction, int threads) {
    const TreeShape shape = count_betting_tree(spot, abstraction);
    const int hands0 = static_cast<int>(hands_in(spot.ranges[0], spot.board).size());
    const int hands1 = static_cast<int>(hands_in(spot.ranges[1], spot.board).size());
    return planned_bytes(shape, layout_for(shape, hands0, hands1, threads), hands0, hands1);
}

PostflopSolver::PostflopSolver(SolveSpot spot, BetAbstraction abstraction, SolverLimits limits, DcfrParams params)
    : limits_(limits), params_(params) {
    if (limits.threads < 0) throw std::invalid_argument("solver: threads must not be negative");
    const TreeShape shape = count_betting_tree(spot, abstraction);
    const std::array<std::vector<int>, 2> combos{hands_in(spot.ranges[0], spot.board),
                                                 hands_in(spot.ranges[1], spot.board)};
    if (combos[0].empty() || combos[1].empty()) throw std::invalid_argument("solver: a range has no hands");
    const int hands0 = static_cast<int>(combos[0].size());
    const int hands1 = static_cast<int>(combos[1].size());

    // The memory check comes before the tree is built.
    const StorageLayout layout = layout_for(shape, hands0, hands1, limits.threads);
    const std::size_t required = planned_bytes(shape, layout, hands0, hands1);
    if (required > limits.memory_bytes) throw SolverMemoryError(required, limits.memory_bytes);

    tree_ = build_betting_tree(spot, abstraction);
    hands_ = MatchupHands(combos[0], combos[1]);
    for (std::size_t p = 0; p < 2; ++p) {
        weights_[p].reserve(combos[p].size());
        for (const int combo : combos[p]) weights_[p].push_back(spot.ranges[p].weight(combo));
    }
    {
        // Some pair of hands must be dealable together, or nothing has a value.
        std::vector<double> faced(static_cast<std::size_t>(hands0));
        fold_values(hands_, 0, weights_[1], 1.0, faced);
        double mass = 0.0;
        for (std::size_t h = 0; h < faced.size(); ++h) mass += weights_[0][h] * faced[h];
        if (!(mass > 0.0)) throw std::invalid_argument("solver: the ranges cannot be dealt together");
    }
    threads_ = layout.threads;
    if (threads_ > 1) pool_ = std::make_unique<WorkerPool>(threads_);
    build_tables(shape.full_boards, shape.checkdown_boards);
    const std::array<std::size_t, 2> n{static_cast<std::size_t>(hands0), static_cast<std::size_t>(hands1)};
    offset_.assign(tree_.nodes.size(), 0);
    std::size_t floats = 0;
    for (std::size_t i = 0; i < tree_.nodes.size(); ++i) {
        const TreeNode& node = tree_.nodes[i];
        if (node.kind != NodeKind::Decision) continue;
        offset_[i] = floats;
        floats += static_cast<std::size_t>(node.child_count) * n[node.player];
    }
    if (floats != layout.floats) throw std::logic_error("solver: the layout does not match the tree");
    regrets_.assign(floats, 0.0f);
    strategy_sum_.assign(floats, 0.0f);
    scratch_.assign(layout.scratch(), 0.0);
    chance_offset_ = layout.main;
    workers_offset_ = layout.main + layout.chance;
    worker_stride_ = layout.stride;
    if (shape.first_layer_cards > 0) {
        parallel_street_ = static_cast<Street>(static_cast<int>(tree_.root().street) + 1);
    }

}

PostflopSolver::~PostflopSolver() = default;

void PostflopSolver::build_tables(std::size_t full_boards, std::size_t checkdown_boards) {
    showdown_of_board_.assign(tree_.boards.size(), -1);
    checkdown_of_board_.assign(tree_.boards.size(), -1);
    std::vector<int> showdown_boards;
    std::vector<int> turn_boards;
    for (std::size_t b = 0; b < tree_.boards.size(); ++b) {
        if (tree_.boards[b].size != 5) continue;
        showdown_of_board_[b] = static_cast<int>(showdown_boards.size());
        showdown_boards.push_back(static_cast<int>(b));
    }
    for (const TreeNode& node : tree_.nodes) {
        const auto b = static_cast<std::size_t>(node.board);
        if (node.kind != NodeKind::CheckDown || checkdown_of_board_[b] >= 0) continue;
        checkdown_of_board_[b] = static_cast<int>(turn_boards.size());
        turn_boards.push_back(node.board);
    }
    if (showdown_boards.size() != full_boards || turn_boards.size() != checkdown_boards) {
        throw std::logic_error("solver: the tables do not match the tree's shape");
    }
    // Reserved first, so the capacities are exact.
    showdowns_.reserve(showdown_boards.size());
    showdowns_.resize(showdown_boards.size());
    checkdowns_.reserve(turn_boards.size());
    checkdowns_.resize(turn_boards.size());
    const auto showdown_count = static_cast<int>(showdown_boards.size());
    const auto task = [&](int k, int) {
        if (k < showdown_count) {
            const auto board = static_cast<std::size_t>(showdown_boards[static_cast<std::size_t>(k)]);
            showdowns_[static_cast<std::size_t>(k)] = ShowdownTable(hands_, tree_.boards[board].view());
        } else {
            const auto i = static_cast<std::size_t>(k - showdown_count);
            const auto board = static_cast<std::size_t>(turn_boards[i]);
            checkdowns_[i] = CheckDownTable(hands_, tree_.boards[board].view());
        }
    };
    const int tasks = showdown_count + static_cast<int>(turn_boards.size());
    if (pool_) {
        pool_->run(tasks, task);
    } else {
        for (int k = 0; k < tasks; ++k) task(k, 0);
    }
}

template <typename W>
void PostflopSolver::attach(W& walker) const {
    walker.tree = &tree_;
    walker.hands = &hands_;
    walker.showdowns = &showdowns_;
    walker.showdown_of_board = &showdown_of_board_;
    walker.checkdowns = &checkdowns_;
    walker.checkdown_of_board = &checkdown_of_board_;
    walker.offset = &offset_;
    walker.parallel_street = parallel_street_;
    walker.pool = pool_.get();
    walker.chance_values = scratch_.data() + chance_offset_;
    walker.worker_scratch = scratch_.data() + workers_offset_;
    walker.worker_stride = worker_stride_;
}

void PostflopSolver::run(int iterations, std::stop_token stop) {
    if (iterations < 0) throw std::invalid_argument("solver: iterations must not be negative");
    const std::size_t widest = static_cast<std::size_t>(std::max(hands_.size(0), hands_.size(1)));
    for (int i = 0; i < iterations; ++i) {
        if (stop.stop_requested()) return;
        ++iterations_;
        Walker walker;
        attach(walker);
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
    attach(walker);
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
    attach(walker);
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
                        capacity_bytes(showdowns_) + capacity_bytes(showdown_of_board_) + capacity_bytes(checkdowns_) +
                        capacity_bytes(checkdown_of_board_) + capacity_bytes(offset_) +
                        capacity_bytes(regrets_) + capacity_bytes(strategy_sum_) + capacity_bytes(scratch_);
    for (const ShowdownTable& table : showdowns_) total += table.bytes();
    for (const CheckDownTable& table : checkdowns_) total += table.bytes();
    return total;
}

}  // namespace poker
