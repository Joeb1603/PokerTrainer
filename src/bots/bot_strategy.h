#pragma once

#include <string>
#include <string_view>

#include "engine/table_types.h"
#include "engine/player_view.h"

namespace poker {

// A bot sees the table only through a PlayerView, so opponent hole cards and the
// undealt board are out of reach by construction. Implementations own their RNG,
// seeded at construction, which keeps them reproducible and independent of the
// engine's shuffle.
class IBotStrategy {
public:
    virtual ~IBotStrategy() = default;

    // Called once before the bot's first decision of each hand.
    virtual void begin_hand(int hand_number) = 0;

    // Requires view.legal to be this seat's mask; the result is always legal for it.
    virtual Action decide(const PlayerView& view) = 0;

    // Safe to show at the table: never reveals hidden state.
    virtual std::string_view name() const = 0;

    // For the hand history only. It may name hidden state, e.g. "Shift:Nit".
    virtual std::string hand_log_label() const { return std::string(name()); }
};

}  // namespace poker
