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

int PlayerView::dead_money() const {
    int antes = 0;
    for (const PublicSeat& s : seats) antes += s.ante_posted;
    return antes;
}

PlayerView make_player_view(const GameState& state, int seat, const LegalActions& legal) {
    if (seat < 0 || seat >= kMaxSeats) throw std::invalid_argument("make_player_view: seat out of range");

    PlayerView view;
    view.seat = seat;
    view.own_hole = state.seats[seat].hole;
    view.position = state.position_of(seat);
    view.hand_number = state.hand_number;
    view.street = state.street;
    view.button = state.button;
    view.small_blind = state.level.small_blind;
    view.big_blind = state.level.big_blind;
    view.ante = state.level.ante;
    view.ante_type = state.level.ante_type;
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
        view.seats[i] = PublicSeat{s.stack, s.street_bet, s.total_bet, s.status, state.position_of(i), s.ante_posted};
    }
    return view;
}

}  // namespace poker
