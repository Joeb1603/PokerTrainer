#include "solver/betting_tree.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
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

class Builder {
public:
    Builder(const BetAbstraction& abstraction, int min_bet) : abstraction_(abstraction), min_bet_(min_bet) {}

    BettingTree& tree() { return tree_; }

    int add_board(const BoardCards& board) {
        CardMask mask = 0;
        for (const Card card : board.view()) mask |= card_mask(card);
        const auto [it, inserted] = board_ids_.try_emplace(mask, static_cast<int>(tree_.boards.size()));
        if (inserted) tree_.boards.push_back(board);
        return it->second;
    }

    int decision(const BuildState& s) {
        const auto p = static_cast<std::size_t>(s.to_act);
        const int current = std::max(s.bet[0], s.bet[1]);
        const int facing = current - s.bet[p];

        std::vector<std::pair<TreeAction, BuildState>> options;
        if (facing > 0) {
            options.emplace_back(TreeAction{ActionType::Fold, s.bet[p], 0, 0.0}, s);
            BuildState called = s;
            called.bet[p] = current;
            called.invested[p] += facing;
            called.pot += facing;
            options.emplace_back(TreeAction{ActionType::Call, current, facing, 0.0}, called);
            if (s.raises < abstraction_.raise_cap && current < s.cap) add_sizes(s, options);
        } else {
            options.emplace_back(TreeAction{ActionType::Check, s.bet[p], 0, 0.0}, s);
            if (current < s.cap) add_sizes(s, options);
        }

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
        BuildState next = s;
        next.street = static_cast<Street>(static_cast<int>(s.street) + 1);
        next.cap = s.cap - s.bet[0];
        next.bet = {0, 0};
        next.to_act = 0;
        next.last_raise = 0;
        next.raises = 0;
        next.check_closes = false;
        next.root = false;
        // All in, or a checked-down river: deal the rest with nobody to act.
        const bool no_betting = next.cap == 0 || (next.street == Street::River && abstraction_.check_down_river);

        const BoardCards board = tree_.boards[static_cast<std::size_t>(s.board)];
        CardMask used = 0;
        for (const Card card : board.view()) used |= card_mask(card);
        std::vector<Card> cards;
        for (int id = 0; id < kDeckSize; ++id) {
            if ((used & (CardMask{1} << id)) == 0) cards.emplace_back(id);
        }

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
            case ActionType::Check: {
                if (s.check_closes) return street_end(s);
                BuildState checked = s;
                checked.to_act = 1 - s.to_act;
                checked.check_closes = true;
                checked.root = false;
                return decision(checked);
            }
            default: return decision(next);
        }
    }

    // Appends the bets or raises, smallest first, as distinct street totals.
    void add_sizes(const BuildState& s, std::vector<std::pair<TreeAction, BuildState>>& options) {
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
    BettingTree tree_;
    std::map<CardMask, int> board_ids_;
};

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

BettingTree build_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction) {
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

    Builder builder(abstraction, root.min_bet);
    BoardCards board;
    for (const Card card : root.board) board.cards[static_cast<std::size_t>(board.size++)] = card;

    BuildState start;
    start.street = board_size == 3 ? Street::Flop : board_size == 4 ? Street::Turn : Street::River;
    start.board = builder.add_board(board);
    start.pot = root.total_pot();
    start.bet = root.street_bet;
    start.cap = std::max(root.street_bet[0], root.street_bet[1]) + root.effective_stack;
    start.to_act = root.to_act;
    start.last_raise = root.last_raise > 0 ? root.last_raise : std::abs(root.street_bet[0] - root.street_bet[1]);
    start.raises = root.raises_made;
    start.check_closes = root.to_act == 1 && root.street_bet[0] == root.street_bet[1];
    start.root = true;
    builder.decision(start);

    BettingTree tree = std::move(builder.tree());
    tree.root_pot = root.total_pot();
    // Exact capacities, so the solver's memory estimate can match them.
    tree.nodes.shrink_to_fit();
    tree.children.shrink_to_fit();
    tree.actions.shrink_to_fit();
    tree.dealt.shrink_to_fit();
    tree.boards.shrink_to_fit();
    return tree;
}

}  // namespace poker
