#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../tools/bench/bench_spots.h"
#include "engine/game_engine.h"
#include "range/combos.h"
#include "range/weighted_range.h"
#include "solver/betting_tree.h"
#include "solver/postflop_solver.h"
#include "solver/spot_from_view.h"

// CMake passes the source directory; the fallback is the repository root as cwd.
#ifndef POKER_SOURCE_DIR
#define POKER_SOURCE_DIR "."
#endif

using namespace poker;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }

// Hole cards for every live player: the named ones, then the lowest unused cards.
ScriptedDeal deal_of(const std::array<std::optional<std::array<Card, 2>>, kMaxSeats>& named,
                     const std::array<Card, 5>& board, const std::vector<int>& live) {
    ScriptedDeal deal;
    deal.board = board;
    CardMask used = card_mask(std::span<const Card>(board));
    for (int i = 0; i < kMaxSeats; ++i) {
        if (!named[static_cast<std::size_t>(i)]) continue;
        deal.holes[static_cast<std::size_t>(i)] = named[static_cast<std::size_t>(i)];
        used |= card_mask(std::span<const Card>(*named[static_cast<std::size_t>(i)]));
    }
    int next = 0;
    const auto spare = [&] {
        while ((used & (CardMask{1} << next)) != 0) ++next;
        used |= CardMask{1} << next;
        return Card(next);
    };
    for (const int i : live) {
        if (!deal.holes[static_cast<std::size_t>(i)]) deal.holes[static_cast<std::size_t>(i)] = std::array<Card, 2>{spare(), spare()};
    }
    return deal;
}

std::array<Card, 5> board_of(const char* a, const char* b, const char* c, const char* d, const char* e) {
    return {card(a), card(b), card(c), card(d), card(e)};
}

const WeightedRange& wide() {
    static const WeightedRange range = WeightedRange::from_hand_range(HandRange::parse("22+, A2s+, K9s+, ATo+, KQo"));
    return range;
}

}  // namespace

// Heads-up: the button posts the small blind, acts first preflop and last after,
// so it is IP. Pot, bets, effective stack and the raise count follow the table.
TEST(SpotFromView, HeadsUpButtonIsInPositionAndChipsMatch) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{1500, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> named{};
    named[0] = hole("Ah", "Kd");
    named[1] = hole("7c", "7d");
    engine.start_hand(deal_of(named, board_of("Qs", "9h", "4c", "2d", "Jc"), {0, 1}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_EQ(engine.state().street, Street::Flop);
    ASSERT_EQ(engine.state().to_act, 1);  // The big blind acts first postflop

    // BB is OOP and first to act on the flop.
    SolveSpot spot = spot_from_view(engine.player_view(1), wide(), wide());
    EXPECT_EQ(solver_player_of(engine.player_view(1)), 0);
    EXPECT_EQ(spot.to_act, 0);
    EXPECT_EQ(spot.pot, 120);
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{0, 0}));
    EXPECT_EQ(spot.effective_stack, 1440);  // The button's 1500 less the 60 preflop
    EXPECT_EQ(spot.raises_made, 0);
    EXPECT_EQ(spot.min_bet, 20);
    EXPECT_EQ(spot.board.size(), 3u);

    // BB bets 40, the button raises to 160: BB faces a raise.
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 40}));
    EXPECT_THROW(spot_from_view(engine.player_view(1), wide(), wide()), std::invalid_argument);  // Not to act
    const PlayerView button = engine.player_view(0);
    spot = spot_from_view(button, wide(), wide());
    EXPECT_EQ(spot.to_act, 1);
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{40, 0}));
    EXPECT_EQ(spot.pot, 120);
    EXPECT_EQ(spot.last_raise, 40);
    EXPECT_EQ(spot.effective_stack, 1400);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 160}));
    spot = spot_from_view(engine.player_view(1), wide(), wide());
    EXPECT_EQ(spot.to_act, 0);
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{40, 160}));
    EXPECT_EQ(spot.last_raise, 120);
    EXPECT_EQ(spot.raises_made, 1);
    EXPECT_EQ(spot.effective_stack, 1280);  // 1440 behind on the flop, less the 160 raise
    EXPECT_EQ(spot.total_pot(), engine.player_view(1).total_pot);

    // The board is out of both ranges.
    for (const auto& range : spot.ranges) {
        for (const Card c : spot.board) {
            for (int other = 0; other < kDeckSize; ++other) {
                if (other == c.id) continue;
                EXPECT_EQ(range.weight(combo_index(c, Card(other))), 0.0f);
            }
        }
    }
}

// Six-max: the player acting last postflop is IP whatever the table index, and a
// folded player's chips on this street stay in the pot.
TEST(SpotFromView, SixMaxOrderAndDeadStreetChips) {
    GameConfig config;
    config.first_button = 0;  // BTN 0, SB 1, BB 2, UTG 3, HJ 4, CO 5
    config.hero_seat = std::nullopt;
    GameEngine engine(config);
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> named{};
    engine.start_hand(deal_of(named, board_of("Kd", "7c", "2h", "4s", "9d"), {0, 1, 2, 3, 4, 5}));
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));       // UTG
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));       // HJ
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));     // CO
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));       // BTN
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));       // SB
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));       // BB
    ASSERT_EQ(engine.state().street, Street::Flop);
    EXPECT_THROW(spot_from_view(engine.player_view(2), wide(), wide()), std::invalid_argument);  // Three-way

    // BB checks, CO bets 40, BTN calls, BB raises to 160 and CO folds: BTN faces BB.
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 40}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 160}));
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_EQ(engine.state().to_act, 0);
    const PlayerView view = engine.player_view(0);
    const SolveSpot spot = spot_from_view(view, wide(), wide());
    EXPECT_EQ(solver_player_of(view), 1);
    EXPECT_EQ(spot.to_act, 1);
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{160, 40}));
    EXPECT_EQ(spot.pot, 160 + 40);  // Three 50s, the small blind and CO's dead 40
    EXPECT_EQ(spot.total_pot(), view.total_pot);
    EXPECT_EQ(spot.effective_stack, 2000 - 50 - 160);
    EXPECT_EQ(spot.raises_made, 1);
    EXPECT_EQ(spot.last_raise, 120);
}

// Uneven stacks: the button overbets 1500 into a big blind with 800 behind. The
// 700 the big blind can't reach is uncalled, so it is in neither the pot nor the
// button's street bet, and the big blind can only call all in or fold.
TEST(SpotFromView, ShortStackFacingOverbetCapsTheUncalledExcess) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{3000, 860, 0, 0, 0, 0};
    GameEngine engine(config);
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> named{};
    engine.start_hand(deal_of(named, board_of("Qs", "9h", "4c", "2d", "Jc"), {0, 1}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_EQ(engine.state().street, Street::Flop);
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 1500}));
    ASSERT_EQ(engine.state().to_act, 1);
    const PlayerView view = engine.player_view(1);
    ASSERT_EQ(view.seats[1].stack, 800);
    ASSERT_EQ(view.seats[0].street_bet, 1500);
    ASSERT_EQ(view.total_pot, 120 + 1500);

    const SolveSpot spot = spot_from_view(view, wide(), wide());
    EXPECT_EQ(spot.to_act, 0);
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{0, 800}));
    EXPECT_EQ(spot.pot, 120);
    EXPECT_EQ(spot.effective_stack, 0);
    EXPECT_EQ(spot.total_pot(), view.total_pot - 700);
    const BettingTree tree = build_betting_tree(spot, river_default_abstraction());
    const auto actions = tree.actions_of(0);
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions[0].type, ActionType::Fold);
    EXPECT_EQ(actions[1].type, ActionType::Call);
}

// The mirror case, where the player to act has the larger street bet and the other
// is all in for less, can't arise heads-up through the engine (an all-in for less
// than a bet only calls and closes the action), so this view is edited by hand:
// hero bet 500, the villain is all in for 300 and hero is to act.
TEST(SpotFromView, OpponentAllInForLessThanHeroBetDropsTheExcess) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> named{};
    engine.start_hand(deal_of(named, board_of("Qs", "9h", "4c", "2d", "Jc"), {0, 1}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_EQ(engine.state().to_act, 1);
    PlayerView view = engine.player_view(1);
    view.seats[1].street_bet = 500;  // Hero, OOP
    view.seats[1].stack = 1440;
    view.seats[0].street_bet = 300;  // Villain, all in
    view.seats[0].stack = 0;
    view.seats[0].status = SeatStatus::AllIn;
    view.total_pot = 120 + 500 + 300;

    const SolveSpot spot = spot_from_view(view, wide(), wide());
    EXPECT_EQ(spot.street_bet, (std::array<int, 2>{300, 300}));
    EXPECT_EQ(spot.pot, 120);
    EXPECT_EQ(spot.effective_stack, 0);
    EXPECT_EQ(spot.total_pot(), 120 + 600);
}

TEST(SpotFromView, RejectsPreflop) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> named{};
    engine.start_hand(deal_of(named, board_of("Kd", "7c", "2h", "4s", "9d"), {0, 1}));
    EXPECT_THROW(spot_from_view(engine.player_view(0), wide(), wide()), std::invalid_argument);
}

TEST(Bench, RangeTextSuitFiltersAndCombos) {
    const WeightedRange range = tools::parse_weighted_range("KQs:0.25, KQs@h, AKo@s:0.5, AhAd:0.75");
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("Kh"), card("Qh"))), 1.0f);
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("Kc"), card("Qc"))), 0.25f);
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("As"), card("Kd"))), 0.5f);
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("Ad"), card("Ks"))), 0.5f);
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("Ad"), card("Kh"))), 0.0f);
    EXPECT_FLOAT_EQ(range.weight(combo_index(card("Ah"), card("Ad"))), 0.75f);
    EXPECT_NEAR(range.total(), 1.0 + 3 * 0.25 + 6 * 0.5 + 0.75, 1e-6);
    EXPECT_THROW(tools::parse_weighted_range("KQs@x"), std::invalid_argument);
    EXPECT_THROW(tools::parse_weighted_range("AhAh"), std::invalid_argument);
    EXPECT_EQ(bench::parse_street_script("BB:X BTN:B33% BB:R120 BTN:A").size(), 4u);
    EXPECT_THROW(bench::parse_street_script("BB:B"), std::invalid_argument);
    EXPECT_THROW(bench::parse_street_script("XX:F"), std::invalid_argument);
    EXPECT_THROW(bench::parse_street_script("BB:X5"), std::invalid_argument);
}

// Every spot in the bench file parses, its script is legal and reaches the hero's
// river decision with the pot and stacks the file states, and both ranges hold
// combos once the board is out.
TEST(Bench, SpotFileParsesAndEverySpotIsLegal) {
    const bench::BenchFile file = bench::load_bench_file(std::string(POKER_SOURCE_DIR) + "/tools/bench/river_spots.json");
    ASSERT_EQ(file.spots.size(), 36u);
    for (const bench::BenchSpot& spot : file.spots) {
        SCOPED_TRACE(spot.id);
        const CardMask board = card_mask(std::span<const Card>(spot.board));
        const std::size_t hero = static_cast<std::size_t>(spot.hero());
        std::array<WeightedRange, 2> ranges = spot.ranges;
        for (WeightedRange& range : ranges) {
            range.remove_cards(board);
            EXPECT_GE(range.total(), 5.0);
        }
        const std::optional<int> hero_combo = bench::first_combo(ranges[hero], board);
        ASSERT_TRUE(hero_combo);
        const std::optional<int> villain_combo = bench::first_combo(ranges[1 - hero], board | combo_mask(*hero_combo));
        ASSERT_TRUE(villain_combo);

        const GameEngine engine =
            bench::replay_spot(file, spot, combo_cards(*hero_combo), combo_cards(*villain_combo));
        const int seat = bench::hero_seat(spot);
        const PlayerView view = engine.player_view(seat);
        EXPECT_EQ(view.street, Street::River);
        EXPECT_EQ(view.total_pot, spot.pot);
        EXPECT_EQ(view.opponents_in_hand(), 1);
        const std::array<int, 2> seats{bench::seat_of(spot.players[0]), bench::seat_of(spot.players[1])};
        int invested = 0;
        for (int i = 0; i < kMaxSeats; ++i) invested += file.stack - view.seats[static_cast<std::size_t>(i)].stack;
        EXPECT_EQ(invested, view.total_pot);
        for (std::size_t p = 0; p < 2; ++p) {
            EXPECT_EQ(view.seats[static_cast<std::size_t>(seats[p])].stack, spot.stacks[p]);
            EXPECT_EQ(view.seats[static_cast<std::size_t>(seats[p])].status, SeatStatus::Active);
        }

        const SolveSpot solve = spot_from_view(view, ranges[hero], ranges[1 - hero]);
        EXPECT_EQ(solve.to_act, spot.hero());
        EXPECT_EQ(solve.total_pot(), spot.pot);
        EXPECT_EQ(solve.street_bet[static_cast<std::size_t>(spot.hero())], 0);
        const int villain_bet = solve.street_bet[1 - hero];
        EXPECT_EQ(villain_bet > 0, spot.hero_ip);
        EXPECT_EQ(solve.effective_stack, std::min(spot.stacks[0] + solve.street_bet[0], spot.stacks[1] + solve.street_bet[1]) -
                                             villain_bet);
        EXPECT_NO_THROW(build_betting_tree(solve, river_default_abstraction()));
    }
}
