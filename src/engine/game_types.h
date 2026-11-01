#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/card.h"
#include "core/hand_evaluator.h"

namespace poker {

inline constexpr int kMaxSeats = 6;

enum class Street : std::uint8_t { Preflop, Flop, Turn, River, Showdown };
enum class Position : std::uint8_t { UTG, HJ, CO, BTN, SB, BB };
enum class SeatStatus : std::uint8_t { Active, Folded, AllIn, Busted };
enum class ActionType : std::uint8_t { Fold, Check, Call, Bet, Raise, AllIn };
enum class GamePhase : std::uint8_t { BetweenHands, Betting, SessionOver };

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

struct Seat {
    int stack = 0;       // Chips behind, excluding anything already bet
    int street_bet = 0;  // Chips put in on the current street
    int total_bet = 0;   // Chips put in over the whole hand
    SeatStatus status = SeatStatus::Active;
    std::optional<std::array<Card, 2>> hole;
    bool acted = false;      // Has taken a voluntary action this street
    int level_acted_at = 0;  // The table's bet level right after that action
    std::optional<int> busted_on_hand;
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

struct Pot {
    int amount = 0;
    std::vector<int> eligible;
};

struct Refund {
    int seat = 0;
    int amount = 0;
};

// Chips won from pots by one seat. Uncalled chips returned are in Refund instead.
struct Payout {
    int seat = 0;
    int amount = 0;
};

struct HandResult {
    std::vector<Pot> pots;
    std::vector<Payout> payouts;
    std::optional<Refund> refund;
    bool showdown = false;
    std::array<std::optional<HandRank>, kMaxSeats> showdown_ranks{};
};

struct GameState {
    GamePhase phase = GamePhase::BetweenHands;
    int hand_number = 0;  // 1-based once a hand has started
    Street street = Street::Preflop;
    int button = -1;
    int small_blind_seat = -1;
    int big_blind_seat = -1;
    int small_blind_posted = 0;
    int big_blind_posted = 0;
    std::optional<int> to_act;
    int current_bet = 0;      // Bet level to match; stays at the big blind if it is posted short
    int last_raise_size = 0;  // Size of the last full bet or raise
    std::array<Seat, kMaxSeats> seats{};
    std::vector<Card> board;
    std::vector<ActionRecord> actions;
    std::array<int, 4> street_start_pot{};  // Pot entering each street (blinds for preflop)
    std::optional<HandResult> last_result;

    int total_pot() const {
        int total = 0;
        for (const Seat& seat : seats) total += seat.total_bet;
        return total;
    }

    // Table position in the current or latest hand; nullopt if not dealt in.
    // Short-handed tables drop positions from the front: 5 players have no UTG,
    // 4 have no HJ. Heads-up, the button is reported as BTN (it also posts SB).
    std::optional<Position> position_of(int seat) const;
};

struct SessionSummary {
    int hands_played = 0;
    std::array<int, kMaxSeats> final_stacks{};
    std::array<std::optional<int>, kMaxSeats> busted_on_hand{};
    std::optional<int> winner;  // Set once only one player has chips left
    bool hero_busted = false;
};

}  // namespace poker
