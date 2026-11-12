#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace poker {

// The vocabulary shared by the engine and the players. Everything here is public
// table information: Seat and GameState, which hold hidden cards, live in
// game_types.h so that bot and advisor code cannot even name them.

inline constexpr int kMaxSeats = 6;

enum class Street : std::uint8_t { Preflop, Flop, Turn, River, Showdown };
enum class Position : std::uint8_t { UTG, HJ, CO, BTN, SB, BB };
enum class SeatStatus : std::uint8_t { Active, Folded, AllIn, Busted };
enum class ActionType : std::uint8_t { Fold, Check, Call, Bet, Raise, AllIn };

constexpr std::string_view to_string(Street street) {
    constexpr std::array<std::string_view, 5> names{"Preflop", "Flop", "Turn", "River", "Showdown"};
    return names[static_cast<std::size_t>(street)];
}

constexpr std::string_view to_string(Position position) {
    constexpr std::array<std::string_view, 6> names{"UTG", "HJ", "CO", "BTN", "SB", "BB"};
    return names[static_cast<std::size_t>(position)];
}

constexpr std::string_view to_string(ActionType type) {
    constexpr std::array<std::string_view, 6> names{"Fold", "Check", "Call", "Bet", "Raise", "AllIn"};
    return names[static_cast<std::size_t>(type)];
}

// Antes are dead money: they never count toward the bet to call, a raise or an
// uncalled refund. PerPlayer: every dealt-in seat posts `ante` before the blinds.
// BigBlind: the big blind posts one ante for the table after its blind (TDA order),
// from whatever its blind leaves.
enum class AnteType : std::uint8_t { None, PerPlayer, BigBlind };

constexpr std::string_view to_string(AnteType type) {
    constexpr std::array<std::string_view, 3> names{"None", "PerPlayer", "BigBlind"};
    return names[static_cast<std::size_t>(type)];
}

// The forced bets of one hand.
struct BlindLevel {
    int small_blind = 10;
    int big_blind = 20;
    int ante = 0;
    AnteType ante_type = AnteType::None;

    bool has_ante() const { return ante > 0 && ante_type != AnteType::None; }
    friend bool operator==(const BlindLevel&, const BlindLevel&) = default;
};

// A level as the settings file and the session log store it: [small blind, big
// blind, ante], the ante 0 when the level has none. The ante's type is the
// schedule's, stored beside the rows.
inline std::array<int, 3> level_row(const BlindLevel& level) {
    return {level.small_blind, level.big_blind, level.has_ante() ? level.ante : 0};
}
inline BlindLevel level_from_row(const std::array<int, 3>& row, AnteType ante_type) {
    return BlindLevel{row[0], row[1], row[2], row[2] > 0 ? ante_type : AnteType::None};
}

// A player's decision. `amount` is used only by Bet and Raise, and is the total
// the player's street bet becomes ("raise to"), not the chips added.
struct Action {
    ActionType type = ActionType::Fold;
    int amount = 0;
};

// What the player to act may do. Bet and Raise are mutually exclusive and share
// min_raise_to / max_raise_to; an amount below the normal minimum is legal only
// when it is the player's whole stack, which min_raise_to already accounts for.
struct LegalActions {
    std::uint8_t mask = 0;
    int call_amount = 0;  // Chips a Call adds (capped at the stack)
    int min_raise_to = 0;
    int max_raise_to = 0;

    constexpr bool allows(ActionType type) const {
        return (mask & (1u << static_cast<unsigned>(type))) != 0;
    }
};

// A voluntary action as it happened. `type` is never AllIn: an all-in resolves
// to Call, Bet or Raise, with the `all_in` flag set on any action that empties
// the player's stack.
struct ActionRecord {
    Street street = Street::Preflop;
    int seat = 0;
    ActionType type = ActionType::Fold;
    int chips_added = 0;
    int street_total = 0;
    bool all_in = false;
};

}  // namespace poker
