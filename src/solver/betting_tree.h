#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/card.h"
#include "engine/table_types.h"

namespace poker {

// A heads-up postflop betting tree. Player 0 is out of position (OOP) and acts
// first on every street; player 1 is in position (IP).

enum class NodeKind : std::uint8_t {
    Decision,  // A player acts: one child per TreeAction
    Chance,    // The next street's card: one child per card not on the board
    Fold,      // TreeNode::player folded
    Showdown,  // Bets are matched on the river (or the street ended with nobody able to bet)
};

// One option at a decision node. Pot fractions follow raise_to_for_pot_fraction:
// a size f makes the street total `bet faced + f * (pot after calling)`, where the
// pot counts every chip in the middle, street bets included.
struct TreeAction {
    ActionType type = ActionType::Check;  // Bet and Raise put in less than the stack; AllIn puts in all of it
    int to = 0;                           // The actor's street total afterwards
    int adds = 0;                         // Chips the action puts in
    double pot_fraction = 0.0;            // Bet, Raise, AllIn: (to - bet faced) / pot after calling, from chips
};

// Pot fractions of the bet and raise sizes on one street.
struct StreetSizes {
    std::vector<double> bets;    // An opening bet, with no bet to face
    std::vector<double> raises;  // Facing a bet or raise
};

struct BetAbstraction {
    std::array<StreetSizes, 3> streets{};  // Flop, turn, river
    // When not empty, replaces the bet sizes (or, facing a bet, the raise sizes) of
    // the root decision only: the advisor's ladder, so its options map onto the tree.
    std::vector<double> root_sizes;
    // Raises per street after the opening bet. At the cap, the player facing a raise
    // can only call or fold.
    int raise_cap = 2;
    bool add_all_in = true;
    // A size that puts in at least this share of the actor's remaining stack
    // becomes all-in: the chips left behind would be too few to matter.
    double all_in_merge = 0.67;
    // Ends the hand at the end of turn betting with no river betting: the river is
    // dealt and checked down. For approximate flop trees; a river tree ignores it.
    bool check_down_river = false;

    const StreetSizes& sizes(Street street) const;
};

// Root bets on the advisor's ladder (25/50/75/100% + all-in), deeper bets 33/75/150%
// + all-in, raises pot + all-in, two raises per street, merge at 0.67 of the stack.
// Only the river sizes are set; flop and turn have none (check or all-in).
BetAbstraction river_default_abstraction();

// The public betting state a tree starts from.
struct BettingRoot {
    std::vector<Card> board;  // 3, 4 or 5 cards; 5 is the river
    int pot = 0;              // Chips in the middle from earlier streets, street bets excluded
    // Chips each player can still add once the larger street bet is matched, so the
    // highest street total either player can reach is max(street_bet) + this. Both
    // players are treated as having that much: chips beyond it could never be called.
    int effective_stack = 0;
    std::array<int, 2> street_bet{};
    int to_act = 0;  // 0 = OOP, 1 = IP. With equal street bets, IP to act means OOP checked.
    // The last full bet or raise increment this street, for the minimum raise.
    // 0 means the difference between the street bets.
    int last_raise = 0;
    int raises_made = 0;  // Raises already made this street (the opening bet is not one)
    int min_bet = 1;      // The smallest opening bet and raise increment: the big blind

    int total_pot() const { return pot + street_bet[0] + street_bet[1]; }
};

struct BoardCards {
    std::array<Card, 5> cards{};
    int size = 0;

    std::span<const Card> view() const { return {cards.data(), static_cast<std::size_t>(size)}; }
};

struct TreeNode {
    NodeKind kind = NodeKind::Showdown;
    std::uint8_t player = 0;  // Decision: the player to act. Fold: the player who folded.
    Street street = Street::River;
    std::uint8_t raises = 0;  // Raises made this street
    int pot = 0;              // Every chip in the middle, street bets included
    std::array<int, 2> street_bet{};
    std::array<int, 2> invested{};  // Chips each player has added since the root
    int board = 0;                  // Index into BettingTree::boards
    int first_child = 0;            // Index into BettingTree::children (and actions, dealt)
    int child_count = 0;
};

// Flat arrays: nodes[0] is the root, and every node comes after its parent
// (depth-first preorder), so a reverse sweep visits children before parents.
// Chance nodes store one child per card, each with its own board, so a turn or
// flop tree's river subtrees are separate nodes that share no state.
struct BettingTree {
    std::vector<TreeNode> nodes;
    std::vector<int> children;
    std::vector<TreeAction> actions;  // Parallel to children: the action leading to each child of a Decision
    std::vector<Card> dealt;          // Parallel to children: the card leading to each child of a Chance
    std::vector<BoardCards> boards;   // boards[0] is the root's; complete boards appear once each
    int root_pot = 0;                 // Every chip in the middle at the root
    int decision_nodes = 0;
    int decision_actions = 0;  // Summed over decision nodes: the (node, action) slots

    const TreeNode& root() const { return nodes.front(); }
    std::span<const int> children_of(int node) const;
    std::span<const TreeAction> actions_of(int node) const;  // Decision nodes
    int child(int node, int index) const;
    std::size_t bytes() const;  // Heap memory held, from capacities
};

// Throws std::invalid_argument for an invalid board, negative chips, a player to
// act who is not facing the larger bet, nothing left to decide (no chips behind and
// no bet to face), or a non-positive or non-finite size or merge threshold.
BettingTree build_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction);

}  // namespace poker
