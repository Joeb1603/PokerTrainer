#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/card.h"
#include "core/hand_evaluator.h"
#include "engine/table_types.h"

namespace poker {

enum class GamePhase : std::uint8_t { BetweenHands, Betting, SessionOver };

struct Seat {
    int stack = 0;       // Chips behind, excluding anything already bet
    int street_bet = 0;  // Chips put in on the current street
    int total_bet = 0;   // Chips put in over the whole hand, antes excluded
    int ante_posted = 0; // Ante paid this hand (dead money; not in total_bet)
    SeatStatus status = SeatStatus::Active;
    std::optional<std::array<Card, 2>> hole;
    bool acted = false;      // Has taken a voluntary action this street
    int level_acted_at = 0;  // The table's bet level right after that action
    std::optional<int> busted_on_hand;
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
    BlindLevel level;  // Blinds and ante of the current or latest hand
    std::optional<int> to_act;
    int current_bet = 0;      // Bet level to match; stays at the big blind if it is posted short
    int last_raise_size = 0;  // Size of the last full bet or raise
    std::array<Seat, kMaxSeats> seats{};
    std::vector<Card> board;
    std::vector<ActionRecord> actions;
    std::array<int, 4> street_start_pot{};  // Pot entering each street (blinds and antes for preflop)
    std::optional<HandResult> last_result;

    // Every chip in the middle, antes included.
    int total_pot() const {
        int total = 0;
        for (const Seat& seat : seats) total += seat.total_bet + seat.ante_posted;
        return total;
    }

    // Antes paid this hand.
    int total_antes() const {
        int total = 0;
        for (const Seat& seat : seats) total += seat.ante_posted;
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
