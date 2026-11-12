#include "core/deck.h"

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "core/random.h"

namespace poker {

Deck::Deck() : Deck(std::span<const Card>{}) {}

Deck::Deck(std::span<const Card> excluded) {
    std::array<bool, kDeckSize> is_excluded{};
    for (const Card card : excluded) is_excluded[card.id] = true;

    for (int id = 0; id < kDeckSize; ++id) {
        if (!is_excluded[static_cast<std::size_t>(id)]) cards_[size_++] = Card(id);
    }
}

void Deck::shuffle(std::mt19937_64& rng) {
    for (std::size_t i = size_; i > next_ + 1; --i) {
        const std::size_t j = next_ + static_cast<std::size_t>(uniform_below(rng, i - next_));
        std::swap(cards_[i - 1], cards_[j]);
    }
}

Card Deck::deal() {
    if (next_ == size_) throw std::out_of_range("Deck::deal: deck is empty");
    return cards_[next_++];
}

Card Deck::deal_random(std::mt19937_64& rng) {
    if (next_ == size_) throw std::out_of_range("Deck::deal_random: deck is empty");
    const std::size_t j = next_ + static_cast<std::size_t>(uniform_below(rng, size_ - next_));
    std::swap(cards_[next_], cards_[j]);
    return cards_[next_++];
}

}  // namespace poker
