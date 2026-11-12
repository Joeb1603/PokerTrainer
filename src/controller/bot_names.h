#pragma once

#include <array>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace poker {

// First names for the random-personalities table: short enough for a seat panel
// (8 letters at most), and chosen to say nothing about how a bot plays.
std::span<const std::string_view> bot_name_pool();

// Five distinct names from the pool, drawn with raw generator bits so a seed gives
// the same names on every standard library.
std::array<std::string, 5> draw_bot_names(std::mt19937_64& rng);

}  // namespace poker
