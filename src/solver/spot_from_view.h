#pragma once

#include "engine/player_view.h"
#include "range/weighted_range.h"
#include "solver/postflop_solver.h"

namespace poker {

// The solver spot for a heads-up postflop decision, built from the public table
// state alone. `view.seat` must be the player to act.
//
// The two players still contesting the pot become OOP (player 0, first to act
// postflop: the first of the two left of the button) and IP (player 1). Heads-up
// the button acts last postflop, so it is IP.
//
// Chips, from the view's public table:
// - pot: every chip in the middle less the two players' bets on this street, so
//   a folded player's chips on this street count as pot
// - street_bet: each player's bet on this street
// - effective_stack: min over the two of (street bet + stack) less the larger
//   street bet, as BettingRoot defines it
// - last_raise: view.last_raise_size; raises_made: the Raise records this street
//   (the opening bet is not a raise); min_bet: the big blind
//
// Ranges: `hero_range` goes to view.seat's side and `villain_range` to the other,
// each with the board cards removed. Nothing else is removed: in particular the
// villain range keeps combos holding hero's own cards, which the caller removes if
// the solve should know them. (The solver already pairs only non-conflicting hands
// in each matchup.)
//
// Throws std::invalid_argument unless it is the flop, turn or river, exactly two
// players are contesting the pot (active or all in), and view.seat is one of them
// and the one to act.
SolveSpot spot_from_view(const PlayerView& view, const WeightedRange& hero_range, const WeightedRange& villain_range);

// The solver player (0 = OOP, 1 = IP) that view.seat is in spot_from_view. Same
// requirements and exceptions.
int solver_player_of(const PlayerView& view);

}  // namespace poker
