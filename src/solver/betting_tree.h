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
    // The turn's betting is over and the river is dealt and checked down (or nobody can
    // bet): a showdown averaged over every river card, without a node per river.
    // TreeNode::board is the turn board. Only in trees whose abstraction sets
    // check_down_river.
    CheckDown,
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
    // dealt and checked down, as one CheckDown node per turn line (showdown averaged
    // over the river cards, card removal exact; no river boards in the tree). For
    // approximate flop trees; a river tree ignores it.
    bool check_down_river = false;

    const StreetSizes& sizes(Street street) const;
};

// The solver tiers. Every one puts the advisor's ladder (25/50/75/100% + all-in) at
// the root decision only, so the advisor's options map onto the tree; deeper sizes
// are coarser to keep the tree small. All merge sizes into all-in at 0.67 of the
// stack. A street with no sizes still has check (or call) and all-in.

// River tier. Root: the ladder. Deeper bets 33/75/150% + all-in, raises pot +
// all-in, two raises per street. Only the river sizes are set; flop and turn have
// none (check or all-in).
BetAbstraction river_default_abstraction();

// Turn tier, for turn roots. Turn: bets 50/100% + all-in, raises pot + all-in, two
// raises. River: bet 66% + all-in, raise all-in only. No flop sizes.
BetAbstraction turn_tier_abstraction();

// Full flop tier, for flop roots: one size per street plus all-in (flop 33%, turn
// 66%, river 75%), raises all-in only.
BetAbstraction flop_full_abstraction();

// Approximate flop tier: flop_full_abstraction on the flop and turn, but the river
// is checked down (check_down_river; no river betting), which makes the tree about
// as small as a turn tree. Advice from it must say "approximate: no river betting".
BetAbstraction flop_checkdown_abstraction();

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

// What build_betting_tree would build, counted without building it: a walk over one
// representative card per chance node (every child of a chance node has the same
// betting, only the card differs) that remembers each street start it has counted,
// so its time and memory grow with the betting lines per street, not with the tree.
// Counts saturate at SIZE_MAX instead of overflowing.
struct TreeShape {
    std::size_t nodes = 0;
    std::size_t children = 0;  // Child slots: entries of children, actions and dealt
    std::size_t decision_nodes = 0;
    std::size_t decision_actions = 0;
    std::array<std::size_t, 2> player_actions{};  // decision_actions by the player to act
    std::size_t boards = 0;
    std::size_t full_boards = 0;       // Boards with 5 cards
    std::size_t checkdown_boards = 0;  // Turn boards that CheckDown nodes end on
    // Scratch for PostflopSolver's walk, in lists of one double per hand of the larger
    // range: what a walk from the root holds at its deepest (a decision node holds
    // 2A + 1 lists, a chance node 3, plus the deepest child).
    std::size_t walk_lists = 0;
    // The first chance layer (the chance nodes dealing the street after the root's),
    // whose children the solver runs in parallel: the most cards any of them deals
    // (0 for a river root) and the most walk_lists below any one card.
    std::size_t first_layer_cards = 0;
    std::size_t first_layer_child_lists = 0;

    std::size_t bytes() const;  // BettingTree::bytes() of the built tree
};

// Same validation and exceptions as build_betting_tree.
TreeShape count_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction);

// Throws std::invalid_argument for an invalid board, negative chips, a player to
// act who is not facing the larger bet, nothing left to decide (no chips behind and
// no bet to face), or a non-positive or non-finite size or merge threshold; and
// std::length_error for a tree with more than INT_MAX nodes or child slots. Counts
// the tree first (count_betting_tree) and reserves every array exactly.
BettingTree build_betting_tree(const BettingRoot& root, const BetAbstraction& abstraction);

}  // namespace poker
