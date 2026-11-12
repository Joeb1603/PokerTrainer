#pragma once

#include <cstdint>
#include <limits>
#include <random>

namespace poker {

// Unbiased integer in [0, bound) by rejection sampling on the raw generator bits.
// Unlike std::uniform_int_distribution, it gives the same values on every standard
// library, so a seed reproduces the same result on every platform. `bound` > 0.
inline std::uint64_t uniform_below(std::mt19937_64& rng, std::uint64_t bound) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t limit = kMax - (kMax % bound + 1) % bound;
    std::uint64_t value = rng();
    while (value > limit) value = rng();
    return value % bound;
}

}  // namespace poker
