#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/card.h"

namespace poker {

using HoleCards = std::array<Card, 2>;

inline constexpr int kDefaultRollouts = 5000;
inline constexpr int kMaxOpponents = 5;

// Rollouts per independently seeded chunk. Chunks, not threads, own the random
// streams, so a result depends only on the seed and the rollout count.
inline constexpr int kEquityChunkRollouts = 4096;

// Below this, automatic threading runs inline. Measured in EquityBench: threads
// gain nothing at 5,000 rollouts (about 1 ms, only two chunks) but 4x by 20,000.
inline constexpr int kEquityParallelThreshold = 20'000;

struct EquityOptions {
    int rollouts = kDefaultRollouts;
    std::uint64_t seed = 0;
    int threads = 0;  // 0 = automatic; otherwise use up to this many threads
};

// All fractions are in [0, 1].
struct EquityResult {
    double win = 0.0;     // Hero wins outright
    double tie = 0.0;     // Hero splits the pot, with any number of players
    double equity = 0.0;  // win + hero's fractional share of the ties
    int rollouts = 0;
    double std_error = 0.0;  // Standard error of `equity`
};

// Hero versus `opponents` (1-5) random hands. `board` holds 0, 3, 4 or 5 cards.
// Throws std::invalid_argument for duplicate or invalid cards, a bad board size,
// a bad opponent count, or non-positive rollouts / negative threads.
EquityResult equity_vs_random(const HoleCards& hero, std::span<const Card> board, int opponents,
                              const EquityOptions& options = {});

// Hero versus specific hands (1-5), with the same validation.
EquityResult equity_vs_hands(const HoleCards& hero, std::span<const Card> board,
                             std::span<const HoleCards> opponents, const EquityOptions& options = {});

}  // namespace poker
