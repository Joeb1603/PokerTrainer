#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/card.h"
#include "strategy/preflop_charts.h"

namespace poker {

// Every two-card starting hand, as an index in [0, 1326).
inline constexpr int kComboCount = kTotalCombos;
static_assert(kComboCount == kDeckSize * (kDeckSize - 1) / 2);

// A set of cards: bit `id` is set for each card held.
using CardMask = std::uint64_t;

constexpr CardMask card_mask(Card card) { return CardMask{1} << card.id; }

constexpr CardMask card_mask(std::span<const Card> cards) {
    CardMask mask = 0;
    for (const Card card : cards) mask |= card_mask(card);
    return mask;
}

// Triangular numbering by card id, so the order of the two cards does not matter:
// with low < high, the index is high * (high - 1) / 2 + low. The cards must differ.
constexpr int combo_index(Card a, Card b) {
    const int low = a.id < b.id ? a.id : b.id;
    const int high = a.id < b.id ? b.id : a.id;
    return high * (high - 1) / 2 + low;
}

// The two cards of a combo, lower card id first. `combo` must be in [0, 1326).
std::array<Card, 2> combo_cards(int combo);
HandClass combo_class(int combo);
CardMask combo_mask(int combo);

// The concrete combos of one class (6, 4 or 12), in increasing index order.
struct ClassCombos {
    std::array<std::uint16_t, 12> combos{};
    int count = 0;

    std::span<const std::uint16_t> view() const { return {combos.data(), static_cast<std::size_t>(count)}; }
};

const ClassCombos& class_combos(HandClass hand);

}  // namespace poker
