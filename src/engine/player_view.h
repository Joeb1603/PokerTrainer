#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/card.h"
#include "engine/table_types.h"

namespace poker {

struct GameState;  // Named only to build a view; players never see its definition

// What everyone at the table can see about a seat. There is deliberately no card
// field of any kind: this is the anti-cheating boundary for bots and the advisor.
struct PublicSeat {
    int stack = 0;
    int street_bet = 0;
    int total_bet = 0;  // Blinds and bets this hand; antes are not included
    SeatStatus status = SeatStatus::Active;
    std::optional<Position> position;  // Nullopt if not dealt in
    int ante_posted = 0;               // Ante paid this hand (dead money)
};

// One player's sanitized snapshot of the table. It owns all of its data, so it
// holds no route back to the engine, its RNG or the undealt board. Showdown
// results are left out too: mucked hands stay private.
struct PlayerView {
    int seat = 0;
    std::optional<std::array<Card, 2>> own_hole;
    std::optional<Position> position;

    int hand_number = 0;
    Street street = Street::Preflop;
    int button = -1;
    int small_blind = 0;
    int big_blind = 0;
    int ante = 0;  // Per seat for PerPlayer, for the table for BigBlind
    AnteType ante_type = AnteType::None;

    std::vector<Card> board;  // Only cards already revealed
    std::optional<int> to_act;
    int current_bet = 0;
    int last_raise_size = 0;
    int total_pot = 0;  // Antes included
    std::array<int, 4> street_start_pot{};
    std::vector<ActionRecord> actions;

    LegalActions legal;  // Empty unless this seat is to act
    std::array<PublicSeat, kMaxSeats> seats{};

    // Other players still contesting the pot (not folded or busted).
    int opponents_in_hand() const;

    // Every ante in the pot, of either type: chips in no seat's total_bet. Zero
    // without antes. (Not what calculate_pots takes as dead money, which is a
    // big-blind ante only; per-player antes are layered there.)
    int dead_money() const;
};

// Copies only public data plus `seat`'s own cards out of `state`, with the blinds
// and ante of the hand. `legal` is the engine's mask for the player to act and is
// dropped for every other seat. Throws std::invalid_argument for a seat out of range.
PlayerView make_player_view(const GameState& state, int seat, const LegalActions& legal);

}  // namespace poker
