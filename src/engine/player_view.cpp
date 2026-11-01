#include "engine/player_view.h"

#include <stdexcept>

#include "engine/game_types.h"

namespace poker {

int PlayerView::opponents_in_hand() const {
    int count = 0;
    for (int other = 0; other < kMaxSeats; ++other) {
        if (other == seat) continue;
        const SeatStatus status = seats[other].status;
        if (status == SeatStatus::Active || status == SeatStatus::AllIn) ++count;
    }
    return count;
}

PlayerView make_player_view(const GameState& state, int seat, int small_blind, int big_blind,
                            const LegalActions& legal) {
    if (seat < 0 || seat >= kMaxSeats) throw std::invalid_argument("make_player_view: seat out of range");

    PlayerView view;
    view.seat = seat;
    view.own_hole = state.seats[seat].hole;
    view.position = state.position_of(seat);
    view.hand_number = state.hand_number;
    view.street = state.street;
    view.button = state.button;
    view.small_blind = small_blind;
    view.big_blind = big_blind;
    view.board = state.board;
    view.to_act = state.to_act;
    view.current_bet = state.current_bet;
    view.last_raise_size = state.last_raise_size;
    view.total_pot = state.total_pot();
    view.street_start_pot = state.street_start_pot;
    view.actions = state.actions;
    if (state.to_act == seat) view.legal = legal;

    for (int i = 0; i < kMaxSeats; ++i) {
        const Seat& s = state.seats[i];
        view.seats[i] = PublicSeat{s.stack, s.street_bet, s.total_bet, s.status, state.position_of(i)};
    }
    return view;
}

}  // namespace poker
