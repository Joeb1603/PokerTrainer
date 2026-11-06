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
// Chips, from the view's public table. A player's reach is street bet + stack,
// the most it can have in this street; cap = min of the two reaches.
// - street_bet: each player's bet on this street, capped at `cap`. A bet beyond
//   the other's reach (an overbet into a shorter stack) is partly uncalled: the
//   engine would return the excess, larger street bet - cap, so it is dropped.
// - pot: every chip in the middle less the uncalled excess and the two (capped)
//   street bets, so a folded player's chips on this street count as pot. Because
//   the excess is dropped from both, pot equals view.total_pot less the two
//   uncapped street bets, and total_pot() is view.total_pot less the excess.
// - effective_stack: cap less the larger (capped) street bet, clamped at 0, as
//   BettingRoot defines it. With a capped bet it is 0: the short player can only
//   call (all in) or fold.
// - last_raise: view.last_raise_size; min_bet: the big blind
// - raises_made: the engine's Raise records this street (the opening bet is not a
//   raise). They include short all-in raises that don't reopen betting, so in
//   those rare spots the tree's raise cap is slightly stricter than the table's.
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
