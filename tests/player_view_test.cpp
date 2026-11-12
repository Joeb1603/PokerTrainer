#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/game_engine.h"
#include "engine/player_view.h"

using namespace poker;

namespace {

// The anti-cheating contract, checked by the compiler. Seat is the control: it
// shows the first concept really does detect a member.
template <class T> concept HasHole = requires(const T& t) { t.hole; };
template <class T> concept HasLastResult = requires(const T& t) { t.last_result; };
template <class T> concept HasPlannedBoard = requires(const T& t) { t.planned_board; };
template <class T> concept HasRng = requires(const T& t) { t.rng; };

static_assert(HasHole<Seat>);
static_assert(!HasHole<PublicSeat>);
static_assert(!HasHole<PlayerView>);
static_assert(!HasLastResult<PlayerView>);
static_assert(!HasPlannedBoard<PlayerView>);
static_assert(!HasRng<PlayerView>);

Card card(const char* text) { return Card::parse(text).value(); }

ScriptedDeal make_deal(std::array<std::array<const char*, 2>, kMaxSeats> holes, std::array<const char*, 5> board) {
    ScriptedDeal deal;
    for (std::size_t seat = 0; seat < holes.size(); ++seat) {
        deal.holes[seat] = std::array<Card, 2>{card(holes[seat][0]), card(holes[seat][1])};
    }
    for (std::size_t i = 0; i < board.size(); ++i) deal.board[i] = card(board[i]);
    return deal;
}

GameConfig fixed_config() {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    return config;
}

// Every field, so two views can be compared for any difference at all.
std::string describe(const PlayerView& v) {
    std::ostringstream out;
    out << v.seat << '|';
    if (v.own_hole) out << (*v.own_hole)[0].to_string() << (*v.own_hole)[1].to_string();
    out << '|' << (v.position ? static_cast<int>(*v.position) : -1) << '|' << v.hand_number << '|'
        << static_cast<int>(v.street) << '|' << v.button << '|' << v.small_blind << '|' << v.big_blind << '|';
    for (const Card c : v.board) out << c.to_string();
    out << '|' << v.to_act.value_or(-1) << '|' << v.current_bet << '|' << v.last_raise_size << '|' << v.total_pot;
    for (const int pot : v.street_start_pot) out << ',' << pot;
    for (const ActionRecord& a : v.actions) {
        out << ';' << static_cast<int>(a.street) << ' ' << a.seat << ' ' << static_cast<int>(a.type) << ' '
            << a.chips_added << ' ' << a.street_total << ' ' << a.all_in;
    }
    out << '|' << static_cast<int>(v.legal.mask) << ' ' << v.legal.call_amount << ' ' << v.legal.min_raise_to << ' '
        << v.legal.max_raise_to;
    for (const PublicSeat& s : v.seats) {
        out << ';' << s.stack << ' ' << s.street_bet << ' ' << s.total_bet << ' ' << static_cast<int>(s.status) << ' '
            << (s.position ? static_cast<int>(*s.position) : -1);
    }
    return out.str();
}

Action random_action(const LegalActions& legal, std::mt19937_64& rng) {
    std::vector<ActionType> options;
    for (const ActionType type : {ActionType::Fold, ActionType::Check, ActionType::Call, ActionType::Bet,
                                  ActionType::Raise, ActionType::AllIn}) {
        if (legal.allows(type)) options.push_back(type);
    }
    const ActionType type = options[static_cast<std::size_t>(rng() % options.size())];
    return {type, legal.min_raise_to};
}

void expect_view_matches_engine(const GameEngine& engine, int seat) {
    const GameState& state = engine.state();
    const PlayerView view = engine.player_view(seat);

    EXPECT_EQ(view.seat, seat);
    EXPECT_EQ(view.own_hole, state.seats[seat].hole);
    EXPECT_EQ(view.position, state.position_of(seat));
    EXPECT_EQ(view.hand_number, state.hand_number);
    EXPECT_EQ(view.street, state.street);
    EXPECT_EQ(view.button, state.button);
    EXPECT_EQ(view.board, state.board);
    EXPECT_EQ(view.to_act, state.to_act);
    EXPECT_EQ(view.current_bet, state.current_bet);
    EXPECT_EQ(view.last_raise_size, state.last_raise_size);
    EXPECT_EQ(view.total_pot, state.total_pot());
    EXPECT_EQ(view.street_start_pot, state.street_start_pot);
    EXPECT_EQ(view.actions.size(), state.actions.size());

    for (int i = 0; i < kMaxSeats; ++i) {
        const Seat& s = state.seats[i];
        EXPECT_EQ(view.seats[i].stack, s.stack);
        EXPECT_EQ(view.seats[i].street_bet, s.street_bet);
        EXPECT_EQ(view.seats[i].total_bet, s.total_bet);
        EXPECT_EQ(view.seats[i].status, s.status);
        EXPECT_EQ(view.seats[i].position, state.position_of(i));
    }

    if (state.to_act == seat) {
        const LegalActions legal = engine.legal_actions();
        EXPECT_EQ(view.legal.mask, legal.mask);
        EXPECT_EQ(view.legal.call_amount, legal.call_amount);
        EXPECT_EQ(view.legal.min_raise_to, legal.min_raise_to);
        EXPECT_EQ(view.legal.max_raise_to, legal.max_raise_to);
    } else {
        EXPECT_EQ(view.legal.mask, 0);
        EXPECT_EQ(view.legal.call_amount, 0);
        EXPECT_EQ(view.legal.min_raise_to, 0);
        EXPECT_EQ(view.legal.max_raise_to, 0);
    }
}

constexpr std::array<std::array<const char*, 2>, kMaxSeats> kHoles{{
    {"As", "Ad"}, {"Ks", "Kd"}, {"Qs", "Qd"}, {"Th", "Td"}, {"8s", "8d"}, {"6s", "6d"},
}};
constexpr std::array<const char*, 5> kBoard{"2c", "7d", "9h", "Js", "3c"};

}  // namespace

TEST(PlayerView, PublicSeatFieldsAreExactlyTheKnownPublicOnes) {
    // A structured binding stops compiling if anyone adds a field, forcing a
    // fresh look at whether the new data is public.
    PublicSeat seat{1, 2, 3, SeatStatus::Folded, Position::CO, 4};
    const auto [stack, street_bet, total_bet, status, position, ante_posted] = seat;
    EXPECT_EQ(stack, 1);
    EXPECT_EQ(street_bet, 2);
    EXPECT_EQ(total_bet, 3);
    EXPECT_EQ(status, SeatStatus::Folded);
    EXPECT_EQ(position, Position::CO);
    EXPECT_EQ(ante_posted, 4);  // Antes are posted in public
}

TEST(PlayerView, RejectsSeatOutOfRange) {
    const GameEngine engine(fixed_config());
    EXPECT_THROW((void)engine.player_view(-1), std::invalid_argument);
    EXPECT_THROW((void)engine.player_view(kMaxSeats), std::invalid_argument);
}

TEST(PlayerView, CarriesBlindSizesAndOwnCards) {
    GameConfig config = fixed_config();
    config.small_blind = 25;
    config.big_blind = 50;
    GameEngine engine(config);
    engine.start_hand(make_deal(kHoles, kBoard));

    const PlayerView view = engine.player_view(3);
    EXPECT_EQ(view.small_blind, 25);
    EXPECT_EQ(view.big_blind, 50);
    ASSERT_TRUE(view.own_hole.has_value());
    EXPECT_EQ((*view.own_hole)[0], card("Th"));
    EXPECT_EQ((*view.own_hole)[1], card("Td"));
    EXPECT_EQ(view.position, Position::UTG);
    EXPECT_EQ(view.opponents_in_hand(), 5);
}

TEST(PlayerView, LegalActionsOnlyForSeatToAct) {
    GameEngine engine(fixed_config());
    engine.start_hand(make_deal(kHoles, kBoard));

    const int to_act = engine.state().to_act.value();
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const PlayerView view = engine.player_view(seat);
        if (seat == to_act) {
            EXPECT_TRUE(view.legal.allows(ActionType::Call));
            EXPECT_EQ(view.legal.call_amount, 20);
        } else {
            EXPECT_EQ(view.legal.mask, 0) << "seat " << seat;
        }
    }
}

TEST(PlayerView, MatchesEngineThroughoutRandomSessions) {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        GameConfig config;
        config.seed = seed;
        config.hero_seat = std::nullopt;
        GameEngine engine(config);
        std::mt19937_64 rng(seed * 977);

        for (int hand = 0; hand < 30 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            while (engine.state().to_act) {
                for (int seat = 0; seat < kMaxSeats; ++seat) expect_view_matches_engine(engine, seat);
                ASSERT_TRUE(engine.apply_action(random_action(engine.legal_actions(), rng)));
            }
            for (int seat = 0; seat < kMaxSeats; ++seat) expect_view_matches_engine(engine, seat);
            if (::testing::Test::HasFailure()) return;
        }
    }
}

// Non-interference: the view must not change when only hidden information does.
// This catches a leak in any field, however it is encoded.
TEST(PlayerView, OpponentCardsAndFutureBoardCannotInfluenceTheView) {
    const auto base = make_deal(kHoles, kBoard);
    // Seat 0 is unchanged; everyone else's cards and all unrevealed board cards differ.
    const auto other = make_deal({{{"As", "Ad"}, {"4s", "4d"}, {"5s", "5d"}, {"Tc", "Ts"}, {"Kh", "Kc"}, {"Qh", "Qc"}}},
                                 {"2c", "7d", "9h", "Ah", "8c"});

    GameEngine a(fixed_config());
    GameEngine b(fixed_config());
    a.start_hand(base);
    b.start_hand(other);

    const std::array<Action, 8> line{{
        {ActionType::Call},             // UTG
        {ActionType::Raise, 80},        // HJ
        {ActionType::Fold},             // CO
        {ActionType::Call},             // BTN (seat 0)
        {ActionType::Fold},             // SB
        {ActionType::Call},             // BB
        {ActionType::Call},             // UTG
        {ActionType::Check},            // BB, first to act on the flop
    }};
    for (const Action& action : line) {
        EXPECT_EQ(describe(a.player_view(0)), describe(b.player_view(0)));
        ASSERT_TRUE(a.apply_action(action));
        ASSERT_TRUE(b.apply_action(action));
    }
    EXPECT_EQ(a.state().street, Street::Flop);
    EXPECT_EQ(describe(a.player_view(0)), describe(b.player_view(0)));

    // The deals really do differ underneath, so the comparison above means something.
    EXPECT_NE(a.state().seats[1].hole, b.state().seats[1].hole);
}

TEST(PlayerView, ShowdownDoesNotRevealOpponentCards) {
    const auto base = make_deal(kHoles, kBoard);
    const auto other = make_deal({{{"As", "Ad"}, {"4s", "4d"}, {"5s", "5d"}, {"Tc", "Ts"}, {"Kh", "Kc"}, {"Qh", "Qc"}}},
                                 kBoard);

    GameEngine a(fixed_config());
    GameEngine b(fixed_config());
    a.start_hand(base);
    b.start_hand(other);

    // Early positions fold; the button and blinds check it down to showdown.
    const auto run = [](GameEngine& engine) {
        for (int i = 0; i < 4; ++i) {
            if (*engine.state().to_act == 0 || *engine.state().to_act == 2) break;
            ASSERT_TRUE(engine.apply_action({ActionType::Fold}));
        }
        while (engine.state().to_act) {
            const LegalActions legal = engine.legal_actions();
            ASSERT_TRUE(engine.apply_action(
                {legal.allows(ActionType::Check) ? ActionType::Check
                 : legal.allows(ActionType::Call) ? ActionType::Call
                                                  : ActionType::Fold}));
        }
    };
    run(a);
    run(b);
    ASSERT_TRUE(a.state().last_result.has_value());
    ASSERT_TRUE(a.state().last_result->showdown);

    ASSERT_TRUE(b.state().last_result->showdown);

    // Seat 0's aces win in both deals, so nothing public differs. The rivals'
    // showdown hands do differ in the engine, and must leave no trace in the view.
    EXPECT_NE(a.state().last_result->showdown_ranks[1], b.state().last_result->showdown_ranks[1]);
    EXPECT_EQ(describe(a.player_view(0)), describe(b.player_view(0)));
    EXPECT_EQ(a.player_view(0).own_hole, a.state().seats[0].hole);
    EXPECT_EQ(a.player_view(0).legal.mask, 0);
}

TEST(PlayerView, ReportsPositionsForShortHandedTables) {
    GameConfig config = fixed_config();
    config.stacks = std::array<int, kMaxSeats>{1000, 1000, 0, 0, 0, 0};
    GameEngine engine(config);
    engine.start_hand();

    EXPECT_EQ(engine.player_view(0).position, Position::BTN);
    EXPECT_EQ(engine.player_view(1).position, Position::BB);
    EXPECT_FALSE(engine.player_view(2).own_hole.has_value());
    EXPECT_FALSE(engine.player_view(2).position.has_value());
    EXPECT_EQ(engine.player_view(0).opponents_in_hand(), 1);
}

TEST(PlayerView, OpponentsInHandExcludesFoldedAndCountsAllIn) {
    GameEngine engine(fixed_config());
    engine.start_hand(make_deal(kHoles, kBoard));

    ASSERT_EQ(engine.state().to_act, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::Fold}));
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn}));

    EXPECT_EQ(engine.player_view(5).opponents_in_hand(), 4);  // Excludes seat 3, includes all-in seat 4
}
