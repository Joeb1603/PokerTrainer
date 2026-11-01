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

}  // namespace poker
