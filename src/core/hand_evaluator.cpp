#include "core/hand_evaluator.h"

#include <phevaluator/phevaluator.h>
#include <phevaluator/rank.h>

#include <algorithm>
#include <stdexcept>

namespace poker {

static_assert(static_cast<int>(HandCategory::StraightFlush) == STRAIGHT_FLUSH);
static_assert(static_cast<int>(HandCategory::HighCard) == HIGH_CARD);

HandRank evaluate5(const std::array<Card, 5>& c) {
    return evaluate_5cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id);
}

HandRank evaluate6(const std::array<Card, 6>& c) {
    return evaluate_6cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id, c[5].id);
}

HandRank evaluate(std::span<const Card> c) {
    switch (c.size()) {
        case 5: return evaluate_5cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id);
        case 6: return evaluate_6cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id, c[5].id);
        case 7: return evaluate_7cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id, c[5].id, c[6].id);
        default: throw std::invalid_argument("evaluate: expected 5, 6 or 7 cards");
    }
}

HandRank evaluate7(const std::array<Card, 7>& c) {
    return evaluate_7cards(c[0].id, c[1].id, c[2].id, c[3].id, c[4].id, c[5].id, c[6].id);
}

HandRank evaluate7(const std::array<Card, 2>& h, const std::array<Card, 5>& b) {
    return evaluate_7cards(h[0].id, h[1].id, b[0].id, b[1].id, b[2].id, b[3].id, b[4].id);
}

HandCategory hand_category(HandRank rank) {
    return static_cast<HandCategory>(get_rank_category(rank));
}

std::string describe_category(HandRank rank) {
    return describe_rank_category(get_rank_category(rank));
}

std::string describe_hand(HandRank rank) {
    return describe_rank(rank);
}

std::string describe_made_hand(HandRank rank) {
    std::string name = describe_hand(rank);
    switch (hand_category(rank)) {
        case HandCategory::TwoPair:
            return "Two Pair, " + name;  // "Aces and Kings"
        case HandCategory::HighCard:
            if (const auto dash = name.find('-'); dash != std::string::npos) name[dash] = ' ';  // "Ace-High"
            return name;
        default:
            return name;
    }
}

std::array<Card, 5> best_five(const std::array<Card, 2>& hole, const std::array<Card, 5>& board) {
    const std::array<Card, 7> all{hole[0], hole[1], board[0], board[1], board[2], board[3], board[4]};
    const HandRank target = evaluate7(all);

    std::array<Card, 5> five{};
    bool found = false;
    for (std::size_t skip_a = 0; skip_a < all.size() && !found; ++skip_a) {
        for (std::size_t skip_b = skip_a + 1; skip_b < all.size() && !found; ++skip_b) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (i != skip_a && i != skip_b) five[n++] = all[i];
            }
            found = evaluate5(five) == target;
        }
    }
    if (!found) throw std::logic_error("best_five: no five-card subset matches the seven-card rank");

    std::array<int, kRankCount> count{};
    for (const Card& c : five) ++count[static_cast<std::size_t>(c.rank())];
    std::sort(five.begin(), five.end(), [&](const Card& a, const Card& b) {
        const int ca = count[static_cast<std::size_t>(a.rank())];
        const int cb = count[static_cast<std::size_t>(b.rank())];
        if (ca != cb) return ca > cb;
        if (a.rank() != b.rank()) return a.rank() > b.rank();
        return a.suit() > b.suit();
    });

    const HandCategory category = hand_category(target);
    const bool straight = category == HandCategory::Straight || category == HandCategory::StraightFlush;
    if (straight && five[0].rank() == Rank::Ace && five[1].rank() == Rank::Five) {
        std::rotate(five.begin(), five.begin() + 1, five.end());  // A-5-4-3-2 reads 5-4-3-2-A
    }
    return five;
}

std::string rank_name(Rank rank) {
    static constexpr std::array<const char*, kRankCount> kNames{
        "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Jack", "Queen", "King", "Ace"};
    return kNames[static_cast<std::size_t>(rank)];
}

std::optional<KickerDifference> kicker_difference(HandRank winner, const std::array<Card, 5>& winner_five,
                                                  HandRank loser, const std::array<Card, 5>& loser_five) {
    if (winner >= loser || describe_made_hand(winner) != describe_made_hand(loser)) return std::nullopt;
    for (std::size_t i = 0; i < winner_five.size(); ++i) {
        if (winner_five[i].rank() != loser_five[i].rank()) {
            return KickerDifference{winner_five[i].rank(), loser_five[i].rank()};
        }
    }
    return std::nullopt;
}

}  // namespace poker
