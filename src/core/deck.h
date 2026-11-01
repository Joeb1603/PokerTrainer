#pragma once

#include <array>
#include <cstddef>
#include <random>
#include <span>

#include "core/card.h"

namespace poker {

class Deck {
public:
    // A full, ordered 52-card deck.
    Deck();

    // An ordered deck without the given cards (for Monte Carlo rollouts around
    // known hole and board cards). Duplicate exclusions are harmless.
    explicit Deck(std::span<const Card> excluded);

    // Shuffles the undealt cards. Uses its own Fisher-Yates rather than
    // std::shuffle, whose output differs between standard libraries, so a seed
    // reproduces the same deal on every platform.
    void shuffle(std::mt19937_64& rng);

    // Removes and returns the next card. Throws std::out_of_range when empty.
    Card deal();

    std::size_t remaining() const { return size_ - next_; }

private:
    std::array<Card, kDeckSize> cards_{};
    std::size_t size_ = 0;
    std::size_t next_ = 0;
};

}  // namespace poker
