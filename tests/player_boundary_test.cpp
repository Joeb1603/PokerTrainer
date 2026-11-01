// This file must see only what a player sees: it includes every header bots and the
// advisor are built from, and nothing from the engine. Seat and GameState hold hidden
// cards, so if any of those headers ever pulls them in, the build breaks here.
#include <gtest/gtest.h>

#include "advisor/training_advisor.h"
#include "bots/bot_strategy.h"
#include "bots/bots.h"
#include "bots/gto_heuristic_engine.h"
#include "engine/player_view.h"
#include "equity/equity.h"
#include "strategy/preflop_charts.h"

namespace poker {
struct Seat;
struct GameState;
class GameEngine;
}  // namespace poker

namespace {

template <typename T>
concept Complete = requires { sizeof(T); };

static_assert(!Complete<poker::Seat>, "player headers must not define Seat");
static_assert(!Complete<poker::GameState>, "player headers must not define GameState");
static_assert(!Complete<poker::GameEngine>, "player headers must not define GameEngine");

// The controls: the same test does see the types a player is meant to have.
static_assert(Complete<poker::PlayerView>);
static_assert(Complete<poker::PublicSeat>);

TEST(PlayerBoundary, ViewIsUsableWithoutEngineHeaders) {
    const poker::PlayerView view;
    EXPECT_FALSE(view.own_hole.has_value());
    EXPECT_EQ(view.opponents_in_hand(), poker::kMaxSeats - 1);  // Default seats are all Active
}

}  // namespace
