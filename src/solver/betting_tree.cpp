#include "solver/betting_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "range/combos.h"

namespace poker {
namespace {

struct BuildState {
    Street street = Street::River;
    int board = 0;
    int pot = 0;
    std::array<int, 2> bet{};
    std::array<int, 2> invested{};
    int cap = 0;  // The highest street total either player can reach
    int to_act = 0;
    int last_raise = 0;
    int raises = 0;
    bool check_closes = false;  // The other player has checked: a check ends the street
    bool root = false;
};

void check_sizes(const std::vector<double>& sizes) {
    for (const double size : sizes) {
        if (!std::isfinite(size) || size <= 0.0) throw std::invalid_argument("betting tree: sizes must be positive");
    }
}

// The betting rules, shared by the builder and the counter so they cannot disagree.
class Rules {
public:
    Rules(const BetAbstraction& abstraction, int min_bet) : abstraction_(abstraction), min_bet_(min_bet) {}

    const BetAbstraction& abstraction() const { return abstraction_; }

    // A decision's options in tree order, each with the state it leads to (for a
    // fold, the state before it).
    std::vector<std::pair<TreeAction, BuildState>> options(const BuildState& s) const {
        const auto p = static_cast<std::size_t>(s.to_act);
        const int current = std::max(s.bet[0], s.bet[1]);
        const int facing = current - s.bet[p];

        std::vector<std::pair<TreeAction, BuildState>> result;
        if (facing > 0) {
            result.emplace_back(TreeAction{ActionType::Fold, s.bet[p], 0, 0.0}, s);
            BuildState called = s;
            called.bet[p] = current;
            called.invested[p] += facing;
            called.pot += facing;
            result.emplace_back(TreeAction{ActionType::Call, current, facing, 0.0}, called);
            if (s.raises < abstraction_.raise_cap && current < s.cap) add_sizes(s, result);
        } else {
            result.emplace_back(TreeAction{ActionType::Check, s.bet[p], 0, 0.0}, s);
            if (current < s.cap) add_sizes(s, result);
        }
        return result;
    }

    // The state after a check that does not close the street.
    static BuildState checked(const BuildState& s) {
        BuildState next = s;
        next.to_act = 1 - s.to_act;
        next.check_closes = true;
        next.root = false;
        return next;
    }

    // The next street's state, before its card is known. `s` is not on the river.
    static BuildState next_street(const BuildState& s) {
        BuildState next = s;
        next.street = static_cast<Street>(static_cast<int>(s.street) + 1);
        next.cap = s.cap - s.bet[0];
        next.bet = {0, 0};
        next.to_act = 0;
        next.last_raise = 0;
        next.raises = 0;
        next.check_closes = false;
        next.root = false;
        return next;
    }

    // Whether the end of this (non-river) street checks the river down in one node.
    bool checks_down(const BuildState& next) const {
        return next.street == Street::River && abstraction_.check_down_river;
    }

private:
    // Appends the bets or raises, smallest first, as distinct street totals.
    void add_sizes(const BuildState& s, std::vector<std::pair<TreeAction, BuildState>>& options) const {
        const auto p = static_cast<std::size_t>(s.to_act);
        const int current = std::max(s.bet[0], s.bet[1]);
        const int facing = current - s.bet[p];
        const int pot_after_call = s.pot + facing;
        const int min_to = current + std::max(s.last_raise, min_bet_);
        const int behind = s.cap - s.bet[p];

        const StreetSizes& street = abstraction_.sizes(s.street);
        const std::vector<double>& sizes = s.root && !abstraction_.root_sizes.empty() ? abstraction_.root_sizes
                                           : current == 0                            ? street.bets
                                                                                     : street.raises;
        std::vector<int> totals;
        for (const double size : sizes) {
            // Compared in double first: a huge size would overflow lround's long (32 bits on MSVC).
            const double raw = size * static_cast<double>(pot_after_call);
            int to = raw >= static_cast<double>(s.cap - current) ? s.cap : current + static_cast<int>(std::lround(raw));
            to = std::max(to, min_to);
            if (to >= s.cap || static_cast<double>(to - s.bet[p]) >= abstraction_.all_in_merge * behind) to = s.cap;
            totals.push_back(to);
        }
        if (abstraction_.add_all_in) totals.push_back(s.cap);
        std::sort(totals.begin(), totals.end());
        totals.erase(std::unique(totals.begin(), totals.end()), totals.end());

        for (const int to : totals) {
            TreeAction action;
            action.type = to == s.cap ? ActionType::AllIn : current == 0 ? ActionType::Bet : ActionType::Raise;
            action.to = to;
            action.adds = to - s.bet[p];
            action.pot_fraction = static_cast<double>(to - current) / static_cast<double>(pot_after_call);

            BuildState next = s;
            next.bet[p] = to;
            next.invested[p] += action.adds;
            next.pot += action.adds;
            next.to_act = 1 - s.to_act;
            next.last_raise = std::max(s.last_raise, to - current);
            next.raises = current == 0 ? s.raises : s.raises + 1;
            next.check_closes = false;
            next.root = false;
            options.emplace_back(action, next);
        }
    }

    const BetAbstraction& abstraction_;
    int min_bet_ = 1;
};

std::vector<Card> cards_not_on(const BoardCards& board) {
    CardMask used = 0;
    for (const Card card : board.view()) used |= card_mask(card);
    std::vector<Card> cards;
    for (int id = 0; id < kDeckSize; ++id) {
        if ((used & (CardMask{1} << id)) == 0) cards.emplace_back(id);
    }
    return cards;
}

class Builder {
public:
    explicit Builder(const Rules& rules) : rules_(rules) {}

    BettingTree& tree() { return tree_; }

    int add_board(const BoardCards& board) {
        CardMask mask = 0;
        for (const Card card : board.view()) mask |= card_mask(card);
        const auto [it, inserted] = board_ids_.try_emplace(mask, static_cast<int>(tree_.boards.size()));
        if (inserted) tree_.boards.push_back(board);
        return it->second;
    }

    int decision(const BuildState& s) {
        const std::vector<std::pair<TreeAction, BuildState>> options = rules_.options(s);
        const int index = add_node(NodeKind::Decision, s, s.to_act);
        const int first = open_children(index, static_cast<int>(options.size()));
        ++tree_.decision_nodes;
        tree_.decision_actions += static_cast<int>(options.size());
        for (std::size_t k = 0; k < options.size(); ++k) {
            const auto slot = static_cast<std::size_t>(first) + k;
            tree_.actions[slot] = options[k].first;
            const int child = after_action(s, options[k].first.type, options[k].second);
            tree_.children[slot] = child;
        }
        return index;
    }

    int street_end(const BuildState& s) {
        if (s.street == Street::River) return add_node(NodeKind::Showdown, s, 0);
        const BuildState next = Rules::next_street(s);
        if (rules_.checks_down(next)) return check_down(s, next);
        const bool no_betting = next.cap == 0;  // All in: deal the rest with nobody to act

        const BoardCards board = tree_.boards[static_cast<std::size_t>(s.board)];
        const std::vector<Card> cards = cards_not_on(board);
        const int index = add_node(NodeKind::Chance, next, 0);
        const int first = open_children(index, static_cast<int>(cards.size()));
        for (std::size_t k = 0; k < cards.size(); ++k) {
            BoardCards dealt = board;
            dealt.cards[static_cast<std::size_t>(dealt.size++)] = cards[k];
            BuildState child = next;
            child.board = add_board(dealt);
            const auto slot = static_cast<std::size_t>(first) + k;
            tree_.dealt[slot] = cards[k];
            const int built = no_betting ? street_end(child) : decision(child);
            tree_.children[slot] = built;
        }
        return index;
    }

private:
    int add_node(NodeKind kind, const BuildState& s, int player) {
        TreeNode node;
        node.kind = kind;
        node.player = static_cast<std::uint8_t>(player);
        node.street = s.street;
        node.raises = static_cast<std::uint8_t>(s.raises);
        node.pot = s.pot;
        node.street_bet = s.bet;
        node.invested = s.invested;
        node.board = s.board;
        tree_.nodes.push_back(node);
        return static_cast<int>(tree_.nodes.size()) - 1;
    }

    // `s` ends the turn; `next` is the river state. The node keeps the turn board.
    int check_down(const BuildState& s, const BuildState& next) {
        BuildState terminal = next;
        terminal.board = s.board;
        return add_node(NodeKind::CheckDown, terminal, 0);
    }

    int open_children(int node, int count) {
        const int first = static_cast<int>(tree_.children.size());
        const auto size = static_cast<std::size_t>(first + count);
        tree_.children.resize(size, -1);
        tree_.actions.resize(size);
        tree_.dealt.resize(size);
        TreeNode& entry = tree_.nodes[static_cast<std::size_t>(node)];
        entry.first_child = first;
        entry.child_count = count;
        return first;
    }

    int after_action(const BuildState& s, ActionType type, const BuildState& next) {
        switch (type) {
            case ActionType::Fold: return add_node(NodeKind::Fold, s, s.to_act);
            case ActionType::Call: return street_end(next);
            case ActionType::Check: return s.check_closes ? street_end(s) : decision(Rules::checked(s));
            default: return decision(next);
        }
    }

    const Rules& rules_;
    BettingTree tree_;
    std::map<CardMask, int> board_ids_;
};

// --- Counting ------------------------------------------------------------------

std::size_t add_sat(std::size_t a, std::size_t b) {
    return a > std::numeric_limits<std::size_t>::max() - b ? std::numeric_limits<std::size_t>::max() : a + b;
}

std::size_t mul_sat(std::size_t a, std::size_t b) {
    if (a == 0 || b == 0) return 0;
    return a > std::numeric_limits<std::size_t>::max() / b ? std::numeric_limits<std::size_t>::max() : a * b;
}

std::size_t choose(std::size_t n, std::size_t k) {
    std::size_t result = 1;
    for (std::size_t i = 1; i <= k; ++i) result = result * (n - k + i) / i;
    return result;
}

// A subtree's counts. Boards are tracked as which deals happen below it.
struct SubtreeCount {
    std::size_t nodes = 0;
    std::size_t children = 0;
    std::size_t decision_nodes = 0;
    std::size_t decision_actions = 0;
    std::array<std::size_t, 2> player_actions{};
    std::size_t walk_lists = 0;
    bool deals_turn = false;   // A chance node deals the turn
    bool deals_river = false;  // A chance node deals the river
    bool checks_down = false;  // A CheckDown node

    void add(const SubtreeCount& child, std::size_t times) {
        nodes = add_sat(nodes, mul_sat(child.nodes, times));
        children = add_sat(children, mul_sat(child.children, times));
        decision_nodes = add_sat(decision_nodes, mul_sat(child.decision_nodes, times));
        decision_actions = add_sat(decision_actions, mul_sat(child.decision_actions, times));
        for (std::size_t p = 0; p < 2; ++p) player_actions[p] = add_sat(player_actions[p], mul_sat(child.player_actions[p], times));
        deals_turn = deals_turn || child.deals_turn;
        deals_river = deals_river || child.deals_river;
        checks_down = checks_down || child.checks_down;
    }
};

class Counter {
public:
    Counter(const Rules& rules, Street root_street, std::size_t root_cards_left)
        : rules_(rules), root_street_(root_street), root_cards_left_(root_cards_left) {}

    std::size_t first_layer_cards = 0;
    std::size_t first_layer_child_lists = 0;

    SubtreeCount decision(const BuildState& s) {
        const std::vector<std::pair<TreeAction, BuildState>> options = rules_.options(s);
        SubtreeCount count;
        count.nodes = 1;
        count.children = options.size();
        count.decision_nodes = 1;
        count.decision_actions = options.size();
        count.player_actions[static_cast<std::size_t>(s.to_act)] = options.size();
        std::size_t deepest = 0;
        for (const auto& [action, next] : options) {
            const SubtreeCount child = after_action(s, action.type, next);
            count.add(child, 1);
            deepest = std::max(deepest, child.walk_lists);
        }
        count.walk_lists = add_sat(2 * options.size() + 1, deepest);
        return count;
    }

private:
    SubtreeCount after_action(const BuildState& s, ActionType type, const BuildState& next) {
        SubtreeCount leaf;
        leaf.nodes = 1;
        switch (type) {
            case ActionType::Fold: return leaf;
            case ActionType::Call: return street_end(next);
            case ActionType::Check: return s.check_closes ? street_end(s) : decision(Rules::checked(s));
            default: return decision(next);
        }
    }

    SubtreeCount street_end(const BuildState& s) {
        SubtreeCount count;
        count.nodes = 1;
        if (s.street == Street::River) return count;
        const BuildState next = Rules::next_street(s);
        if (rules_.checks_down(next)) {
            count.checks_down = true;
            return count;
        }
        // Cards left once this street's card is dealt: one fewer per street dealt since the root.
        const auto dealt_before = static_cast<std::size_t>(static_cast<int>(s.street) - static_cast<int>(root_street_));
        const std::size_t cards = root_cards_left_ - dealt_before;

        const auto key = std::make_tuple(static_cast<int>(next.street), next.pot, next.cap);
        auto found = memo_.find(key);
        if (found == memo_.end()) {
            const SubtreeCount child = next.cap == 0 ? street_end(next) : decision(next);
            found = memo_.emplace(key, child).first;
        }
        const SubtreeCount& child = found->second;
        count.children = cards;
        count.add(child, cards);
        count.walk_lists = add_sat(3, child.walk_lists);
        if (next.street == Street::Turn) count.deals_turn = true;
        if (next.street == Street::River) count.deals_river = true;
        if (static_cast<int>(next.street) == static_cast<int>(root_street_) + 1) {
            first_layer_cards = std::max(first_layer_cards, cards);
            first_layer_child_lists = std::max(first_layer_child_lists, child.walk_lists);
        }
        return count;
    }

    const Rules& rules_;
    Street root_street_;
    std::size_t root_cards_left_;
    // Street starts already counted, by street, pot and cap: the subtree below a
    // street start depends on nothing else.
    std::map<std::tuple<int, int, int>, SubtreeCount> memo_;
};

void validate(const BettingRoot& root, const BetAbstraction& abstraction) {
    const std::size_t board_size = root.board.size();
    if (board_size < 3 || board_size > 5) throw std::invalid_argument("betting tree: board must have 3, 4 or 5 cards");
    CardMask used = 0;
    for (const Card card : root.board) {
        if (card.id >= kDeckSize || (used & card_mask(card)) != 0) {
            throw std::invalid_argument("betting tree: duplicate or invalid board card");
        }
        used |= card_mask(card);
    }
    if (root.pot < 0 || root.effective_stack < 0 || root.street_bet[0] < 0 || root.street_bet[1] < 0 ||
        root.last_raise < 0 || root.raises_made < 0) {
        throw std::invalid_argument("betting tree: chips must not be negative");
    }
    if (root.total_pot() <= 0) throw std::invalid_argument("betting tree: the pot must be positive");
    if (root.min_bet < 1) throw std::invalid_argument("betting tree: the minimum bet must be positive");
    if (root.to_act != 0 && root.to_act != 1) throw std::invalid_argument("betting tree: to_act must be 0 or 1");
    const auto p = static_cast<std::size_t>(root.to_act);
    if (root.street_bet[p] > root.street_bet[1 - p]) {
        throw std::invalid_argument("betting tree: the player to act must not have the larger bet");
    }
    if (root.effective_stack == 0 && root.street_bet[0] == root.street_bet[1]) {
        throw std::invalid_argument("betting tree: nothing to decide");
    }
    if (abstraction.raise_cap < 0) throw std::invalid_argument("betting tree: raise cap must not be negative");
    if (!std::isfinite(abstraction.all_in_merge) || abstraction.all_in_merge <= 0.0) {
        throw std::invalid_argument("betting tree: the all-in merge threshold must be positive");
    }
    check_sizes(abstraction.root_sizes);
    for (const StreetSizes& street : abstraction.streets) {
        check_sizes(street.bets);
        check_sizes(street.raises);
    }
}

Street root_street(const BettingRoot& root) {
    const std::size_t board_size = root.board.size();
    return board_size == 3 ? Street::Flop : board_size == 4 ? Street::Turn : Street::River;
}

BuildState start_state(const BettingRoot& root, int board) {
    BuildState start;
    start.street = root_street(root);
    start.board = board;
    start.pot = root.total_pot();
    start.bet = root.street_bet;
    start.cap = std::max(root.street_bet[0], root.street_bet[1]) + root.effective_stack;
    start.to_act = root.to_act;
    start.last_raise = root.last_raise > 0 ? root.last_raise : std::abs(root.street_bet[0] - root.street_bet[1]);
    start.raises = root.raises_made;
    start.check_closes = root.to_act == 1 && root.street_bet[0] == root.street_bet[1];
    start.root = true;
    return start;
}

template <typename T>
std::size_t capacity_bytes(const std::vector<T>& values) {
    return values.capacity() * sizeof(T);
}

}  // namespace

const StreetSizes& BetAbstraction::sizes(Street street) const {
    if (street < Street::Flop || street > Street::River) throw std::invalid_argument("bet abstraction: postflop only");
    return streets[static_cast<std::size_t>(street) - 1];
}

BetAbstraction river_default_abstraction() {
    BetAbstraction abstraction;
    abstraction.streets[2].bets = {0.33, 0.75, 1.5};
    abstraction.streets[2].raises = {1.0};
    abstraction.root_sizes = {0.25, 0.5, 0.75, 1.0};
    return abstraction;
}

BetAbstraction turn_tier_abstraction() {
    BetAbstraction abstraction;
    abstraction.streets[1].bets = {0.5, 1.0};
    abstraction.streets[1].raises = {1.0};
    abstraction.streets[2].bets = {0.66};
    abstraction.root_sizes = {0.25, 0.5, 0.75, 1.0};
    return abstraction;
}

BetAbstraction flop_full_abstraction() {
    BetAbstraction abstraction;
    abstraction.streets[0].bets = {0.33};
    abstraction.streets[1].bets = {0.66};
    abstraction.streets[2].bets = {0.75};
    abstraction.root_sizes = {0.25, 0.5, 0.75, 1.0};
    return abstraction;
}

BetAbstraction flop_checkdown_abstraction() {
    BetAbstraction abstraction = flop_full_abstraction();
    abstraction.streets[2] = {};
    abstraction.check_down_river = true;
    return abstraction;
}

std::span<const int> BettingTree::children_of(int node) const {
    const TreeNode& entry = nodes[static_cast<std::size_t>(node)];
    return {children.data() + entry.first_child, static_cast<std::size_t>(entry.child_count)};
}

std::span<const TreeAction> BettingTree::actions_of(int node) const {
    const TreeNode& entry = nodes[static_cast<std::size_t>(node)];
    return {actions.data() + entry.first_child, static_cast<std::size_t>(entry.child_count)};
}

int BettingTree::child(int node, int index) const {
    return children[static_cast<std::size_t>(nodes[static_cast<std::size_t>(node)].first_child + index)];
}

std::size_t BettingTree::bytes() const {
    return capacity_bytes(nodes) + capacity_bytes(children) + capacity_bytes(actions) + capacity_bytes(dealt) +
           capacity_bytes(boards);
}

std::size_t TreeShape::bytes() const {
    const std::size_t slot = sizeof(int) + sizeof(TreeAction) + sizeof(Card);
    std::size_t total = mul_sat(nodes, sizeof(TreeNode));
    total = add_sat(total, mul_sat(children, slot));
    return add_sat(total, mul_sat(boards, sizeof(BoardCards)));
}

TreeShape count_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction) {
    validate(root, abstraction);
    const Rules rules(abstraction, root.min_bet);
    const std::size_t board_size = root.board.size();
    const std::size_t left = static_cast<std::size_t>(kDeckSize) - board_size;
    Counter counter(rules, root_street(root), left);
    const SubtreeCount count = counter.decision(start_state(root, 0));

    TreeShape shape;
    shape.nodes = count.nodes;
    shape.children = count.children;
    shape.decision_nodes = count.decision_nodes;
    shape.decision_actions = count.decision_actions;
    shape.player_actions = count.player_actions;
    shape.walk_lists = count.walk_lists;
    shape.first_layer_cards = counter.first_layer_cards;
    shape.first_layer_child_lists = counter.first_layer_child_lists;
    // Every chance node deals every card left, so the boards d cards past the root
    // are all C(left, d) of them once any chance node deals that street. A CheckDown
    // node ends on a turn board: below a turn chance node, or at a turn root.
    shape.boards = 1;
    if (board_size == 3 && count.deals_turn) shape.boards += left;
    if (board_size < 5 && count.deals_river) shape.full_boards = choose(left, 5 - board_size);
    if (board_size == 5) shape.full_boards = 1;
    if (board_size < 5) shape.boards += shape.full_boards;
    if (count.checks_down) shape.checkdown_boards = board_size == 3 ? left : 1;
    return shape;
}

BettingTree build_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction) {
    const TreeShape shape = count_betting_tree(root, abstraction);
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (shape.nodes > kMax || shape.children > kMax || shape.decision_actions > kMax) {
        throw std::length_error("betting tree: too many nodes");
    }

    const Rules rules(abstraction, root.min_bet);
    Builder builder(rules);
    BettingTree& building = builder.tree();
    building.nodes.reserve(shape.nodes);
    building.children.reserve(shape.children);
    building.actions.reserve(shape.children);
    building.dealt.reserve(shape.children);
    building.boards.reserve(shape.boards);

    BoardCards board;
    for (const Card card : root.board) board.cards[static_cast<std::size_t>(board.size++)] = card;
    builder.decision(start_state(root, builder.add_board(board)));

    BettingTree tree = std::move(building);
    tree.root_pot = root.total_pot();
    if (tree.nodes.size() != shape.nodes || tree.children.size() != shape.children ||
        tree.boards.size() != shape.boards || static_cast<std::size_t>(tree.decision_actions) != shape.decision_actions) {
        throw std::logic_error("betting tree: the count does not match the tree");
    }
    // Exact capacities, so the solver's memory estimate can match them (a no-op
    // after the exact reserves).
    tree.nodes.shrink_to_fit();
    tree.children.shrink_to_fit();
    tree.actions.shrink_to_fit();
    tree.dealt.shrink_to_fit();
    tree.boards.shrink_to_fit();
    return tree;
}

}  // namespace poker
