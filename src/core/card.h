#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace poker {

enum class Rank : std::uint8_t {
    Two, Three, Four, Five, Six, Seven, Eight, Nine, Ten, Jack, Queen, King, Ace
};

enum class Suit : std::uint8_t { Clubs, Diamonds, Hearts, Spades };

inline constexpr int kRankCount = 13;
inline constexpr int kSuitCount = 4;
inline constexpr int kDeckSize = 52;

// A playing card stored as `rank * 4 + suit` (0 = 2c ... 51 = As), which is the
// id encoding the phevaluator library expects.
struct Card {
    std::uint8_t id = 0;

    constexpr Card() = default;
    constexpr explicit Card(int card_id) : id(static_cast<std::uint8_t>(card_id)) {}
    constexpr Card(Rank r, Suit s)
        : id(static_cast<std::uint8_t>(static_cast<int>(r) * kSuitCount + static_cast<int>(s))) {}

    constexpr Rank rank() const { return static_cast<Rank>(id / kSuitCount); }
    constexpr Suit suit() const { return static_cast<Suit>(id % kSuitCount); }

    constexpr auto operator<=>(const Card&) const = default;

    // Parses two-character notation such as "As" or "Td". Rank and suit are
    // case-insensitive. Returns nullopt for anything else.
    static std::optional<Card> parse(std::string_view text);

    // Canonical two-character form, e.g. "As".
    std::string to_string() const;
};

}  // namespace poker
