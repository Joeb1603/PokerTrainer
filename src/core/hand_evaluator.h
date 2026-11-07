#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "core/card.h"

namespace poker {

// Hand strength: lower is stronger.
using HandRank = int;
inline constexpr HandRank kBestHandRank = 1;     // Royal flush
inline constexpr HandRank kWorstHandRank = 7462; // 7-5-4-3-2 offsuit, 5-card only

enum class HandCategory {
    StraightFlush = 1,
    FourOfAKind,
    FullHouse,
    Flush,
    Straight,
    ThreeOfAKind,
    TwoPair,
    OnePair,
    HighCard,
};

// Rank of the best five-card hand among the given distinct cards. Passing
// duplicate cards is undefined: the result is not meaningful.
HandRank evaluate5(const std::array<Card, 5>& cards);  // Flop: hole + 3 board
HandRank evaluate6(const std::array<Card, 6>& cards);  // Turn: hole + 4 board
HandRank evaluate7(const std::array<Card, 7>& cards);  // River: hole + 5 board
HandRank evaluate7(const std::array<Card, 2>& hole, const std::array<Card, 5>& board);

// Dispatches on size, for street-agnostic code holding hole + current board.
// Throws std::invalid_argument unless there are 5, 6 or 7 cards.
HandRank evaluate(std::span<const Card> cards);

// `rank` must be in [kBestHandRank, kWorstHandRank].
HandCategory hand_category(HandRank rank);

// Category name for a hand, e.g. "Full House".
std::string describe_category(HandRank rank);

// Specific hand name, e.g. "Aces Full over Kings".
std::string describe_hand(HandRank rank);

// The name to show a player for their current hand, e.g. "Ace High" or
// "Two Pair, Aces and Kings". Logs keep using describe_hand.
std::string describe_made_hand(HandRank rank);

// The five cards that make the best hand from hole + board, most significant first:
// larger rank groups first, then higher ranks, with the ace last in a five-high
// straight. For "Kc Kd As 9d 7h" with any two lower cards: K K A 9 7.
std::array<Card, 5> best_five(const std::array<Card, 2>& hole, const std::array<Card, 5>& board);

// Singular rank name, e.g. "Ace", "Ten", "Two".
std::string rank_name(Rank rank);

// Where a better hand beats a worse one that shows the same describe_made_hand
// name, such as two "Pair of Kings": the first card ranks, most significant first,
// at which their best five cards differ. Nullopt when the names differ or the hands
// tie. Each `five` must come from best_five for the matching rank.
struct KickerDifference {
    Rank winner;
    Rank loser;
};
std::optional<KickerDifference> kicker_difference(HandRank winner, const std::array<Card, 5>& winner_five,
                                                  HandRank loser, const std::array<Card, 5>& loser_five);

}  // namespace poker
