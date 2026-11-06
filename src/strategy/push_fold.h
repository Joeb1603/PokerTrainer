#pragma once

#include <cstdint>
#include <optional>

#include "engine/table_types.h"
#include "strategy/preflop_charts.h"

namespace poker {

// Push/fold tables for short effective stacks, blinds 0.5/1 BB and no ante,
// solved by tools/gen_push_fold.cpp with fictitious play over the 169 classes
// (all-in equities from range/preflop_matrix.h, card removal between the jammer
// and each caller from combo counts). The advisor and range model use them; the
// bots never do.
//
// Every player is assumed to hold the same stack of stack_bb big blinds. First
// in, the jammer faces each player behind as a possible lone caller: the first
// to call is the only one (overcalls are ignored), and each caller's range beats
// folding heads-up against the jam. The SB-versus-BB game is the heads-up game,
// so heads-up (players == 2) the button, which posts the small blind, uses the
// SB tables. A short-handed table drops the early positions, so a position keeps
// the same number of players behind and the same tables.
inline constexpr int kPushFoldMinStackBb = 3;
inline constexpr int kPushFoldMaxStackBb = 15;

// First in (no shover): Raise (all in) or Fold, for UTG through SB. Facing a lone
// jam from `shover`, an earlier position: Call or Fold. Nullopt for a stack
// outside [kPushFoldMinStackBb, kPushFoldMaxStackBb], for the BB first in, and for
// a hero who does not act after the shover.
std::optional<PreflopResponse> push_fold_response(HandClass hand, Position hero, int stack_bb,
                                                  std::optional<Position> shover, int players = kMaxSeats);

// The jam range first in from `jammer`, or the call range of `caller` against a
// lone jam from `jammer`, with the same nullopt cases as push_fold_response.
std::optional<HandRange> push_fold_jam_range(Position jammer, int stack_bb, int players = kMaxSeats);
std::optional<HandRange> push_fold_call_range(Position caller, Position jammer, int stack_bb,
                                              int players = kMaxSeats);

// Fictitious-play iterations behind the tables.
int push_fold_iterations();

namespace detail {

inline constexpr int kPushFoldStackCount = kPushFoldMaxStackBb - kPushFoldMinStackBb + 1;
inline constexpr int kPushFoldJammerCount = 5;  // UTG, HJ, CO, BTN, SB
inline constexpr int kPushFoldPositionCount = 6;
inline constexpr int kHandSetWords = 3;  // 169 bits

// Bit k (word k / 64, bit k % 64) is set for the class whose HandClass::index()
// is k. Indexed by Position, then stack_bb - kPushFoldMinStackBb; a call entry
// whose caller does not act after the jammer is empty. Defined in
// strategy/generated/push_fold_data.cpp.
extern const std::uint64_t kPushFoldJam[kPushFoldJammerCount][kPushFoldStackCount][kHandSetWords];
extern const std::uint64_t kPushFoldCall[kPushFoldJammerCount][kPushFoldPositionCount][kPushFoldStackCount]
                                        [kHandSetWords];
extern const int kPushFoldIterations;

}  // namespace detail
}  // namespace poker
