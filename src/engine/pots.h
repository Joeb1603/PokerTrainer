#pragma once

#include <array>
#include <optional>
#include <vector>

#include "engine/game_types.h"

namespace poker {

struct PotBreakdown {
    std::vector<Pot> pots;  // Main pot first, then side pots
    std::optional<Refund> refund;
};

// Splits hand contributions into a main pot and side pots.
//
// `contributed` is each seat's total for the hand; `live` marks seats that have
// not folded. Chips only one player put in were never called, so they come back
// as a refund instead of forming a pot. Layers with no live contributor merge
// into the pot below them, since folded chips stay in play for whoever is left.
PotBreakdown calculate_pots(const std::array<int, kMaxSeats>& contributed,
                            const std::array<bool, kMaxSeats>& live);

// The pot as it stands mid-hand, for display. pots plus uncalled always equals total.
struct LivePots {
    int total = 0;
    std::vector<Pot> pots;  // Main pot first; side pots only exist because of all-ins
    int uncalled = 0;       // Chips of a live player nobody has matched so far
};

// Unlike calculate_pots, a difference in bets alone does not split the pot, because
// players with chips behind can still match it. Pots split only at the amount a
// player is all-in for. `eligible` counts seats that can still win that pot, including
// those who have yet to call.
LivePots calculate_live_pots(const std::array<int, kMaxSeats>& contributed,
                             const std::array<SeatStatus, kMaxSeats>& statuses);

}  // namespace poker
