#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/card.h"
#include "core/hand_evaluator.h"

namespace poker {

// The hands each player can hold in a solve: combo indices, fixed for its
// lifetime. A hand is referred to by its position in its player's list.
class MatchupHands {
public:
    MatchupHands() = default;
    // Each list ascending and without repeats; throws std::invalid_argument otherwise.
    MatchupHands(std::span<const int> combos0, std::span<const int> combos1);

    int size(int player) const { return static_cast<int>(combos_[static_cast<std::size_t>(player)].size()); }
    std::span<const int> combos(int player) const { return combos_[static_cast<std::size_t>(player)]; }
    // Card ids, lower first.
    const std::array<std::uint8_t, 2>& cards(int player, int hand) const {
        return cards_[static_cast<std::size_t>(player)][static_cast<std::size_t>(hand)];
    }
    // The opponent's hand with exactly the same two cards, or -1.
    int same_combo(int player, int hand) const {
        return same_[static_cast<std::size_t>(player)][static_cast<std::size_t>(hand)];
    }
    // The player's hands holding card `card_id`, ascending.
    std::span<const int> with_card(int player, int card_id) const;

    std::size_t bytes() const;  // From capacities
    static std::size_t bytes_for(int hands0, int hands1);

private:
    std::array<std::vector<int>, 2> combos_;
    std::array<std::vector<std::array<std::uint8_t, 2>>, 2> cards_;
    std::array<std::vector<int>, 2> same_;
    // Hands by card: with_card(p, c) is by_card_[p][starts_[p][c] .. starts_[p][c+1]).
    std::array<std::vector<int>, 2> by_card_;
    std::array<std::array<int, kDeckSize + 1>, 2> starts_{};
};

// Values for `player`'s hands against the opponent's reach on a terminal node. Each
// entry of `out` is a sum over the opponent's hands o that share no card with the
// hand, so it is counterfactual: weighted by reach, not normalised.

// out[h] = payoff * (reach of the opponent hands h does not block). O(n): one pass
// gathers the total and per-card reach, and each hand subtracts its two cards'
// sums and adds back the one opponent combo with both of its cards, which both
// subtractions removed.
void fold_values(const MatchupHands& hands, int player, std::span<const double> opp_reach, double payoff,
                 std::span<double> out);

// Every player's hands ranked on one complete board, built once per board.
class ShowdownTable {
public:
    ShowdownTable(const MatchupHands& hands, std::span<const Card> board);

    // out[h] = sum over unblocked o of opp_reach[o] * (win, tie or lose payoff of h
    // against o). Hands holding a board card get 0. `hands` must be the list the
    // table was built from. O(n): the opponent's hands sorted by rank are swept
    // once from the weak end, gathering the reach a hand beats (in total and per
    // card, so the hand's own blockers come off), and once from the strong end for
    // the reach it loses to; ties are what is left of the unblocked reach. The
    // opponent combo with the hand's own two cards ties it, so it is never in the
    // win or loss sums and needs no add-back there.
    void values(const MatchupHands& hands, int player, std::span<const double> opp_reach, double win, double tie,
                double lose, std::span<double> out) const;

    std::size_t bytes() const;  // From capacities
    static std::size_t bytes_for(int hands0, int hands1);

private:
    // Per player: hands that hold no board card, weakest first (ties by hand index),
    // and their ranks in the same order.
    std::array<std::vector<int>, 2> order_;
    std::array<std::vector<HandRank>, 2> rank_;
};

// O(n^2) versions for tests.
void fold_values_reference(const MatchupHands& hands, int player, std::span<const double> opp_reach, double payoff,
                           std::span<double> out);
void showdown_values_reference(const MatchupHands& hands, std::span<const Card> board, int player,
                               std::span<const double> opp_reach, double win, double tie, double lose,
                               std::span<double> out);

}  // namespace poker
