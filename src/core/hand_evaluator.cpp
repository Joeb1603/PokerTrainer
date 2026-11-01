#include "core/hand_evaluator.h"

#include <phevaluator/phevaluator.h>
#include <phevaluator/rank.h>

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

}  // namespace poker
