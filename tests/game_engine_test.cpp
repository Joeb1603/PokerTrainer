#include <gtest/gtest.h>

#include <random>
#include <set>
#include <stdexcept>
#include <string>

#include "engine/game_engine.h"

using namespace poker;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

// A rainbow, unpaired board that gives no straight or flush with the holes below.
constexpr std::array<const char*, 5> kDryBoard{"2c", "7d", "9h", "Js", "3c"};

std::array<Card, 5> dry_board() {
    return {card(kDryBoard[0]), card(kDryBoard[1]), card(kDryBoard[2]), card(kDryBoard[3]), card(kDryBoard[4])};
}

// Hole cards strongest to weakest by index, all pocket pairs over the dry board.
ScriptedDeal strength_order_deal(std::array<int, kMaxSeats> strength_of_seat) {
    constexpr std::array<std::array<const char*, 2>, kMaxSeats> pairs{{
        {"As", "Ad"}, {"Ks", "Kd"}, {"Qs", "Qd"}, {"Th", "Td"}, {"8s", "8d"}, {"6s", "6d"},
    }};
    ScriptedDeal deal;
    deal.board = dry_board();
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const auto& pair = pairs[strength_of_seat[seat]];
        deal.holes[seat] = std::array<Card, 2>{card(pair[0]), card(pair[1])};
    }
    return deal;
}

GameConfig config_with(std::array<int, kMaxSeats> stacks, int button = 0) {
    GameConfig config;
    config.stacks = stacks;
    config.first_button = button;
    config.hero_seat = std::nullopt;
    return config;
}

void act(GameEngine& engine, int expected_seat, Action action) {
    ASSERT_TRUE(engine.state().to_act.has_value());
    ASSERT_EQ(*engine.state().to_act, expected_seat);
    ASSERT_TRUE(engine.apply_action(action)) << "seat " << expected_seat << " " << to_string(action.type);
}

void fold(GameEngine& e, int seat) { act(e, seat, {ActionType::Fold}); }
void check(GameEngine& e, int seat) { act(e, seat, {ActionType::Check}); }
void call(GameEngine& e, int seat) { act(e, seat, {ActionType::Call}); }
void bet(GameEngine& e, int seat, int to) { act(e, seat, {ActionType::Bet, to}); }
void raise_to(GameEngine& e, int seat, int to) { act(e, seat, {ActionType::Raise, to}); }
void all_in(GameEngine& e, int seat) { act(e, seat, {ActionType::AllIn}); }

int stack_total(const GameEngine& e) {
    int total = 0;
    for (const Seat& s : e.state().seats) total += s.stack;
    return total;
}

constexpr std::array<int, kMaxSeats> kSixEqual{2000, 2000, 2000, 2000, 2000, 2000};
constexpr std::array<int, kMaxSeats> kThreeHanded{1000, 1000, 1000, 0, 0, 0};

}  // namespace

// --- Setup and blinds --------------------------------------------------------

TEST(GameEngine, RejectsBadConfigs) {
    GameConfig bad_blinds;
    bad_blinds.small_blind = 30;
    EXPECT_THROW(GameEngine{bad_blinds}, std::invalid_argument);

    GameConfig lonely = config_with({100, 0, 0, 0, 0, 0});
    EXPECT_THROW(GameEngine{lonely}, std::invalid_argument);

    GameConfig busted_button = config_with({100, 100, 0, 0, 0, 0}, 3);
    EXPECT_THROW(GameEngine{busted_button}, std::invalid_argument);
}

TEST(GameEngine, DefaultConfigMatchesSpec) {
    const GameConfig config;
    EXPECT_EQ(config.starting_stack, 2000);
    EXPECT_EQ(config.small_blind, 10);
    EXPECT_EQ(config.big_blind, 20);
}

TEST(GameEngine, PostsBlindsAndActionStartsUnderTheGun) {
    GameEngine e(config_with(kSixEqual, 0));
    e.start_hand();
    const GameState& s = e.state();

    EXPECT_EQ(s.hand_number, 1);
    EXPECT_EQ(s.small_blind_seat, 1);
    EXPECT_EQ(s.big_blind_seat, 2);
    EXPECT_EQ(s.seats[1].stack, 1990);
    EXPECT_EQ(s.seats[2].stack, 1980);
    EXPECT_EQ(s.total_pot(), 30);
    EXPECT_EQ(s.to_act, 3);
    EXPECT_EQ(s.street, Street::Preflop);
}

TEST(GameEngine, SixMaxPositions) {
    GameEngine e(config_with(kSixEqual, 2));
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.position_of(2), Position::BTN);
    EXPECT_EQ(s.position_of(3), Position::SB);
    EXPECT_EQ(s.position_of(4), Position::BB);
    EXPECT_EQ(s.position_of(5), Position::UTG);
    EXPECT_EQ(s.position_of(0), Position::HJ);
    EXPECT_EQ(s.position_of(1), Position::CO);
}

TEST(GameEngine, ShortHandedPositionsDropFromTheFront) {
    GameEngine four(config_with({500, 500, 500, 500, 0, 0}, 0));
    four.start_hand();
    EXPECT_EQ(four.state().position_of(3), Position::CO);
    EXPECT_FALSE(four.state().position_of(4).has_value());

    GameEngine two(config_with({500, 500, 0, 0, 0, 0}, 0));
    two.start_hand();
    EXPECT_EQ(two.state().position_of(0), Position::BTN);
    EXPECT_EQ(two.state().position_of(1), Position::BB);
}

TEST(GameEngine, DealsDistinctCardsAndSameSeedReproduces) {
    GameConfig config;
    config.seed = 99;
    GameEngine first(config);
    GameEngine second(config);
    first.start_hand();
    second.start_hand();

    std::set<Card> dealt;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const auto hole = first.state().seats[seat].hole;
        ASSERT_TRUE(hole);
        dealt.insert((*hole)[0]);
        dealt.insert((*hole)[1]);
        EXPECT_EQ(hole, second.state().seats[seat].hole);
    }
    EXPECT_EQ(dealt.size(), 12u);
    EXPECT_EQ(first.state().button, second.state().button);
}

TEST(GameEngine, ScriptWithDuplicateCardsIsRejected) {
    GameEngine e(config_with(kThreeHanded));
    ScriptedDeal deal = strength_order_deal({0, 1, 2, 3, 4, 5});
    deal.holes[1] = deal.holes[0];
    EXPECT_THROW(e.start_hand(deal), std::invalid_argument);
}

TEST(GameEngine, StartHandDuringHandThrows) {
    GameEngine e(config_with(kSixEqual));
    e.start_hand();
    EXPECT_THROW(e.start_hand(), std::logic_error);
}

// --- Rotation and heads-up ---------------------------------------------------

TEST(GameEngine, ButtonRotatesClockwise) {
    GameEngine e(config_with(kSixEqual, 4));
    e.start_hand();
    EXPECT_EQ(e.state().button, 4);
    // Everyone folds to the big blind (seat 0, after SB seat 5). UTG is seat 1.
    fold(e, 1); fold(e, 2); fold(e, 3); fold(e, 4); fold(e, 5);
    EXPECT_EQ(e.state().phase, GamePhase::BetweenHands);

    e.start_hand();
    EXPECT_EQ(e.state().button, 5);
    EXPECT_EQ(e.state().small_blind_seat, 0);
    EXPECT_EQ(e.state().big_blind_seat, 1);
}

TEST(GameEngine, BustedSeatsAreSkippedForButtonAndBlindsAndDeals) {
    GameEngine e(config_with({1000, 0, 1000, 0, 1000, 0}, 0));
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.small_blind_seat, 2);
    EXPECT_EQ(s.big_blind_seat, 4);
    EXPECT_EQ(s.to_act, 0);
    EXPECT_FALSE(s.seats[1].hole);
    EXPECT_EQ(s.seats[1].status, SeatStatus::Busted);

    fold(e, 0); fold(e, 2);
    e.start_hand();
    EXPECT_EQ(s.button, 2);
    EXPECT_EQ(s.small_blind_seat, 4);
    EXPECT_EQ(s.big_blind_seat, 0);
}

TEST(GameEngine, HeadsUpButtonPostsSmallBlindAndActsFirstPreflopLastPostflop) {
    GameEngine e(config_with({1000, 0, 0, 1000, 0, 0}, 3));
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.small_blind_seat, 3);
    EXPECT_EQ(s.big_blind_seat, 0);
    EXPECT_EQ(s.seats[3].street_bet, 10);
    EXPECT_EQ(s.seats[0].street_bet, 20);

    call(e, 3);
    check(e, 0);
    EXPECT_EQ(s.street, Street::Flop);
    check(e, 0);  // Big blind first postflop
    check(e, 3);
    EXPECT_EQ(s.street, Street::Turn);
    EXPECT_EQ(s.to_act, 0);
}

TEST(GameEngine, TransitionFromThreeToHeadsUpUsesHeadsUpBlinds) {
    // Seat 0 busts on hand one, leaving seats 1 and 2.
    GameEngine e(config_with({100, 1000, 1000, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({5, 0, 1, 2, 3, 4}));
    all_in(e, 0);
    call(e, 1);
    call(e, 2);
    check(e, 1); check(e, 2);
    check(e, 1); check(e, 2);
    check(e, 1); check(e, 2);
    ASSERT_EQ(e.state().seats[0].status, SeatStatus::Busted);

    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.button, 1);
    EXPECT_EQ(s.small_blind_seat, 1);
    EXPECT_EQ(s.big_blind_seat, 2);
    EXPECT_EQ(s.to_act, 1);
}

// --- Legal action masks and raise sizing -------------------------------------

TEST(GameEngine, OpeningLegalActions) {
    GameEngine e(config_with(kSixEqual));
    e.start_hand();
    const LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Fold));
    EXPECT_TRUE(legal.allows(ActionType::Call));
    EXPECT_TRUE(legal.allows(ActionType::Raise));
    EXPECT_TRUE(legal.allows(ActionType::AllIn));
    EXPECT_FALSE(legal.allows(ActionType::Check));
    EXPECT_FALSE(legal.allows(ActionType::Bet));
    EXPECT_EQ(legal.call_amount, 20);
    EXPECT_EQ(legal.min_raise_to, 40);
    EXPECT_EQ(legal.max_raise_to, 2000);
}

TEST(GameEngine, BigBlindHasOptionWhenLimpedTo) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    call(e, 0);
    call(e, 1);
    ASSERT_EQ(e.state().to_act, 2);
    EXPECT_EQ(e.state().street, Street::Preflop);

    const LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Check));
    EXPECT_TRUE(legal.allows(ActionType::Raise));
    EXPECT_FALSE(legal.allows(ActionType::Call));
    EXPECT_EQ(legal.min_raise_to, 40);
    check(e, 2);
    EXPECT_EQ(e.state().street, Street::Flop);
}

TEST(GameEngine, MinimumRaiseFollowsLastRaiseSize) {
    GameEngine e(config_with(kSixEqual));
    e.start_hand();
    raise_to(e, 3, 60);  // Raise of 40
    EXPECT_EQ(e.legal_actions().min_raise_to, 100);
    raise_to(e, 4, 200);  // Raise of 140
    EXPECT_EQ(e.legal_actions().min_raise_to, 340);
    EXPECT_EQ(e.legal_actions().call_amount, 200);
}

TEST(GameEngine, IllegalActionsAreRejectedWithoutStateChange) {
    GameEngine e(config_with(kSixEqual));
    e.start_hand();
    const GameState before = e.state();

    EXPECT_FALSE(e.apply_action({ActionType::Check}));
    EXPECT_FALSE(e.apply_action({ActionType::Bet, 100}));
    EXPECT_FALSE(e.apply_action({ActionType::Raise, 39}));
    EXPECT_FALSE(e.apply_action({ActionType::Raise, 2001}));

    EXPECT_EQ(e.state().to_act, before.to_act);
    EXPECT_EQ(e.state().seats[3].stack, before.seats[3].stack);
    EXPECT_TRUE(e.state().actions.empty());
}

TEST(GameEngine, NoActionsOutsideAHand) {
    GameEngine e(config_with(kSixEqual));
    EXPECT_EQ(e.legal_actions().mask, 0);
    EXPECT_FALSE(e.apply_action({ActionType::Fold}));
}

TEST(GameEngine, PostflopBetSizing) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    call(e, 0); call(e, 1); check(e, 2);
    ASSERT_EQ(e.state().street, Street::Flop);
    EXPECT_EQ(e.state().board.size(), 3u);
    EXPECT_EQ(e.state().to_act, 1);  // Left of the button

    LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Bet));
    EXPECT_TRUE(legal.allows(ActionType::Check));
    EXPECT_FALSE(legal.allows(ActionType::Raise));
    EXPECT_EQ(legal.min_raise_to, 20);
    EXPECT_FALSE(e.apply_action({ActionType::Bet, 19}));

    bet(e, 1, 50);
    legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Raise));
    EXPECT_EQ(legal.min_raise_to, 100);
    EXPECT_EQ(legal.call_amount, 50);
}

TEST(GameEngine, MinimumRaiseResetsEachStreet) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    raise_to(e, 0, 200);
    call(e, 1);
    call(e, 2);
    ASSERT_EQ(e.state().street, Street::Flop);
    EXPECT_EQ(e.legal_actions().min_raise_to, 20);
}

TEST(GameEngine, StreetsDealCorrectBoardCountsAndRecordPots) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand(strength_order_deal({0, 1, 2, 3, 4, 5}));
    const GameState& s = e.state();
    EXPECT_TRUE(s.board.empty());
    call(e, 0); call(e, 1); check(e, 2);
    EXPECT_EQ(s.board.size(), 3u);
    check(e, 1); check(e, 2); check(e, 0);
    EXPECT_EQ(s.board.size(), 4u);
    check(e, 1); check(e, 2); check(e, 0);
    EXPECT_EQ(s.board.size(), 5u);
    EXPECT_EQ(s.street, Street::River);
    EXPECT_EQ(s.board[0], card("2c"));
    EXPECT_EQ(s.board[4], card("3c"));
    EXPECT_EQ(s.street_start_pot[0], 30);
    EXPECT_EQ(s.street_start_pot[1], 60);
    EXPECT_EQ(s.street_start_pot[3], 60);
}

// --- Short all-ins and reopening ---------------------------------------------

TEST(GameEngine, ShortAllInRaiseDoesNotReopenBettingForPriorActor) {
    GameEngine e(config_with({1000, 150, 1000, 0, 0, 0}, 0));
    e.start_hand();
    raise_to(e, 0, 100);  // Raise of 80
    all_in(e, 1);         // To 150: only 50 more, below a full raise

    // Seat 2 has not acted, so can raise, and the minimum is still based on 80.
    LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Raise));
    EXPECT_EQ(legal.min_raise_to, 230);
    call(e, 2);

    // Seat 0 has acted and only faces 50 more: call or fold.
    legal = e.legal_actions();
    EXPECT_EQ(e.state().to_act, 0);
    EXPECT_TRUE(legal.allows(ActionType::Call));
    EXPECT_TRUE(legal.allows(ActionType::Fold));
    EXPECT_FALSE(legal.allows(ActionType::Raise));
    EXPECT_FALSE(legal.allows(ActionType::AllIn));
    EXPECT_FALSE(e.apply_action({ActionType::Raise, 300}));
    EXPECT_FALSE(e.apply_action({ActionType::AllIn}));
}

TEST(GameEngine, ShortAllInBetDoesNotReopenForPlayerWhoChecked) {
    GameEngine e(config_with({1000, 1000, 35, 0, 0, 0}, 0));
    e.start_hand();
    call(e, 0); call(e, 1); check(e, 2);
    ASSERT_EQ(e.state().street, Street::Flop);
    check(e, 1);
    all_in(e, 2);  // Bets 15, below the 20 minimum

    // Seat 0 has not acted: may raise, with the minimum based on the big blind.
    LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Raise));
    EXPECT_EQ(legal.min_raise_to, 35);
    call(e, 0);

    // Seat 1 checked, and a 15 bet is not a full bet: call or fold only.
    legal = e.legal_actions();
    EXPECT_EQ(e.state().to_act, 1);
    EXPECT_FALSE(legal.allows(ActionType::Raise));
    EXPECT_EQ(legal.call_amount, 15);
}

TEST(GameEngine, CumulativeShortAllInsReopenBetting) {
    // Button 0, SB 1, BB 2, UTG 3.
    GameEngine e(config_with({140, 190, 1000, 1000, 0, 0}, 0));
    e.start_hand();
    raise_to(e, 3, 100);  // Raise of 80
    all_in(e, 0);         // To 140: +40
    all_in(e, 1);         // To 190: +50, short alone, but +90 in total
    call(e, 2);

    ASSERT_EQ(e.state().to_act, 3);
    EXPECT_TRUE(e.legal_actions().allows(ActionType::Raise));
}

TEST(GameEngine, FullRaiseReopensBetting) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    raise_to(e, 0, 100);
    raise_to(e, 1, 180);  // A full raise of 80
    call(e, 2);
    ASSERT_EQ(e.state().to_act, 0);
    EXPECT_TRUE(e.legal_actions().allows(ActionType::Raise));
    EXPECT_EQ(e.legal_actions().min_raise_to, 260);
}

TEST(GameEngine, ShortCallGoesAllInForLess) {
    GameEngine e(config_with({1000, 50, 1000, 0, 0, 0}, 0));
    e.start_hand();
    raise_to(e, 0, 200);

    const LegalActions legal = e.legal_actions();
    EXPECT_EQ(legal.call_amount, 40);  // 50 stack less the 10 small blind
    EXPECT_TRUE(legal.allows(ActionType::Call));
    EXPECT_TRUE(legal.allows(ActionType::AllIn));
    EXPECT_FALSE(legal.allows(ActionType::Raise));

    call(e, 1);
    EXPECT_EQ(e.state().seats[1].status, SeatStatus::AllIn);
    EXPECT_TRUE(e.state().actions.back().all_in);
    EXPECT_EQ(e.state().actions.back().type, ActionType::Call);
    EXPECT_EQ(e.state().current_bet, 200);
}

TEST(GameEngine, AllInResolvesToBetOrRaiseInActionLog) {
    GameEngine e(config_with({1000, 1000, 300, 0, 0, 0}, 0));
    e.start_hand();
    call(e, 0); call(e, 1); check(e, 2);
    check(e, 1);
    all_in(e, 2);
    EXPECT_EQ(e.state().actions.back().type, ActionType::Bet);
    EXPECT_TRUE(e.state().actions.back().all_in);
    all_in(e, 0);
    EXPECT_EQ(e.state().actions.back().type, ActionType::Raise);
    EXPECT_EQ(e.state().actions.back().chips_added, 980);
}

TEST(GameEngine, RaiseEqualToStackIsAnAllIn) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    raise_to(e, 0, 1000);
    EXPECT_EQ(e.state().seats[0].status, SeatStatus::AllIn);
    EXPECT_TRUE(e.state().actions.back().all_in);
}

TEST(GameEngine, CannotRaiseWhenNoOpponentCanRespond) {
    GameEngine e(config_with({1000, 200, 0, 0, 0, 0}, 0));
    e.start_hand();  // Heads-up: seat 0 SB/button, seat 1 BB
    all_in(e, 0);
    const LegalActions legal = e.legal_actions();
    EXPECT_EQ(e.state().to_act, 1);
    EXPECT_FALSE(legal.allows(ActionType::Raise));
}

// --- Short blinds and automatic runouts --------------------------------------

TEST(GameEngine, ShortBigBlindStillSetsFullCallAmount) {
    GameEngine e(config_with({1000, 1000, 5, 1000, 0, 0}, 0));
    e.start_hand(strength_order_deal({1, 2, 0, 3, 4, 5}));
    const GameState& s = e.state();
    EXPECT_EQ(s.big_blind_posted, 5);
    EXPECT_EQ(s.seats[2].status, SeatStatus::AllIn);
    EXPECT_EQ(s.current_bet, 20);
    EXPECT_EQ(s.to_act, 3);
    EXPECT_EQ(e.legal_actions().call_amount, 20);
    EXPECT_EQ(e.legal_actions().min_raise_to, 40);

    call(e, 3); call(e, 0); call(e, 1);
    // Big blind is all-in; the others play on. Check down.
    for (int street = 0; street < 3; ++street) {
        check(e, 1); check(e, 3); check(e, 0);
    }
    // Seat 2 (aces) wins only the main pot of 5 x 4; seat 0 (kings) the side pot.
    EXPECT_EQ(s.seats[2].stack, 20);
    EXPECT_EQ(s.seats[0].stack, 980 + 45);
    EXPECT_EQ(stack_total(e), 3005);
}

TEST(GameEngine, ShortSmallBlindPostsWhatItHas) {
    GameEngine e(config_with({1000, 4, 1000, 0, 0, 0}, 0));
    e.start_hand();
    EXPECT_EQ(e.state().small_blind_posted, 4);
    EXPECT_EQ(e.state().seats[1].status, SeatStatus::AllIn);
    EXPECT_EQ(e.state().to_act, 0);
}

TEST(GameEngine, BlindAllInWithBigBlindCoveringRunsOutWithoutAction) {
    // Heads-up: button/SB has only 5; BB covers and owes nothing more.
    GameEngine e(config_with({5, 100, 0, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({0, 1, 2, 3, 4, 5}));
    const GameState& s = e.state();

    EXPECT_FALSE(s.to_act);
    ASSERT_TRUE(s.last_result);
    EXPECT_TRUE(s.last_result->showdown);
    EXPECT_EQ(s.board.size(), 5u);
    ASSERT_TRUE(s.last_result->refund);
    EXPECT_EQ(s.last_result->refund->seat, 1);
    EXPECT_EQ(s.last_result->refund->amount, 15);
    EXPECT_EQ(s.seats[0].stack, 10);
    EXPECT_EQ(s.seats[1].stack, 95);
}

TEST(GameEngine, ShortBigBlindHeadsUpSmallBlindMustStillDecide) {
    GameEngine e(config_with({1000, 5, 0, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({0, 1, 2, 3, 4, 5}));
    const GameState& s = e.state();

    ASSERT_EQ(s.to_act, 0);
    const LegalActions legal = e.legal_actions();
    EXPECT_TRUE(legal.allows(ActionType::Call));
    EXPECT_FALSE(legal.allows(ActionType::Raise));
    call(e, 0);

    EXPECT_EQ(s.board.size(), 5u);
    EXPECT_EQ(s.seats[0].stack, 1005);
    EXPECT_EQ(s.seats[1].status, SeatStatus::Busted);
}

TEST(GameEngine, AllInCallRunsOutRemainingStreets) {
    GameEngine e(config_with({500, 300, 0, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({1, 0, 2, 3, 4, 5}));
    all_in(e, 0);
    call(e, 1);

    const GameState& s = e.state();
    EXPECT_EQ(s.board.size(), 5u);
    EXPECT_EQ(s.last_result->refund->seat, 0);
    EXPECT_EQ(s.last_result->refund->amount, 200);
    EXPECT_EQ(s.seats[1].stack, 600);
    EXPECT_EQ(s.seats[0].stack, 200);
    EXPECT_EQ(s.phase, GamePhase::BetweenHands);
}

TEST(GameEngine, RunoutStopsNoEarlierThanActionLeftOnStreet) {
    // One player all-in on the flop, two others keep betting into a side pot.
    GameEngine e(config_with({1000, 100, 1000, 0, 0, 0}, 0));
    e.start_hand();
    raise_to(e, 0, 100);
    all_in(e, 1);
    call(e, 2);
    ASSERT_EQ(e.state().street, Street::Flop);
    EXPECT_EQ(e.state().to_act, 2);  // Seat 1 is all-in and skipped
    bet(e, 2, 100);
    call(e, 0);
    EXPECT_EQ(e.state().street, Street::Turn);
}

// --- Winning and side pots ---------------------------------------------------

TEST(GameEngine, FoldToWalkGivesBigBlindTheBlindsWithoutShowdown) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    fold(e, 0);
    fold(e, 1);

    const GameState& s = e.state();
    ASSERT_TRUE(s.last_result);
    EXPECT_FALSE(s.last_result->showdown);
    EXPECT_TRUE(s.board.empty());
    EXPECT_EQ(s.seats[2].stack, 1010);
    EXPECT_EQ(s.seats[1].stack, 990);
    ASSERT_EQ(s.last_result->payouts.size(), 1u);
    EXPECT_EQ(s.last_result->payouts[0].seat, 2);
    EXPECT_EQ(s.last_result->payouts[0].amount, 20);
    EXPECT_EQ(s.last_result->refund->amount, 10);
}

TEST(GameEngine, FoldOnLaterStreetStopsDealing) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    call(e, 0); call(e, 1); check(e, 2);
    bet(e, 1, 40);
    fold(e, 2);
    fold(e, 0);
    EXPECT_EQ(e.state().board.size(), 3u);
    EXPECT_EQ(e.state().seats[1].stack, 1040);
}

TEST(GameEngine, BestHandWinsAtShowdown) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand(strength_order_deal({2, 0, 1, 3, 4, 5}));
    call(e, 0); call(e, 1); check(e, 2);
    for (int street = 0; street < 3; ++street) {
        check(e, 1); check(e, 2); check(e, 0);
    }
    const GameState& s = e.state();
    EXPECT_EQ(s.street, Street::Showdown);
    EXPECT_TRUE(s.last_result->showdown);
    EXPECT_EQ(s.seats[1].stack, 1040);
    EXPECT_EQ(s.seats[0].stack, 980);
    EXPECT_EQ(hand_category(*s.last_result->showdown_ranks[1]), HandCategory::OnePair);
    EXPECT_FALSE(s.last_result->showdown_ranks[3]);
}

TEST(GameEngine, ThreeWayAllInPaysMainAndSidePotsToDifferentWinners) {
    GameEngine e(config_with({100, 300, 500, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({0, 1, 2, 3, 4, 5}));
    all_in(e, 0);
    all_in(e, 1);
    EXPECT_FALSE(e.apply_action({ActionType::AllIn}));  // Covers both all-ins: nobody left to raise
    call(e, 2);

    const GameState& s = e.state();
    ASSERT_TRUE(s.last_result);
    ASSERT_EQ(s.last_result->pots.size(), 2u);
    EXPECT_EQ(s.last_result->pots[0].amount, 300);
    EXPECT_EQ(s.last_result->pots[1].amount, 400);
    EXPECT_EQ(s.seats[0].stack, 300);  // Aces take the main pot
    EXPECT_EQ(s.seats[1].stack, 400);  // Kings take the side pot
    EXPECT_EQ(s.seats[2].stack, 200);  // Queens get the uncalled 200 back
}

TEST(GameEngine, ShortStackWinningMainPotLeavesSidePotToBestOfRest) {
    GameEngine e(config_with({1000, 1000, 50, 1000, 0, 0}, 0));
    e.start_hand(strength_order_deal({2, 1, 0, 3, 4, 5}));
    raise_to(e, 3, 200);
    call(e, 0);
    fold(e, 1);
    // Seat 2, the big blind, shoves its last 30 for less.
    all_in(e, 2);
    const GameState& s = e.state();
    ASSERT_EQ(s.street, Street::Flop);
    ASSERT_EQ(s.seats[2].status, SeatStatus::AllIn);
    // Main pot: 50 x 3 + 10 from the folded small blind = 160.
    for (int street = 0; street < 3; ++street) {
        check(e, 3); check(e, 0);
    }
    EXPECT_EQ(s.seats[2].stack, 160);
    // Side pot: 150 x 2 = 300 to queens (seat 0) over tens (seat 3).
    EXPECT_EQ(s.seats[0].stack, 800 + 300);
    EXPECT_EQ(s.seats[3].stack, 800);
    EXPECT_EQ(stack_total(e), 3050);
}

TEST(GameEngine, TiedHandsSplitPotAndOddChipGoesLeftOfButton) {
    // Blinds 5/10 with a dead small blind makes the pot odd: 10 + 10 + 5 = 25.
    GameConfig config = config_with(kThreeHanded);
    config.small_blind = 5;
    config.big_blind = 10;
    GameEngine e(config);

    ScriptedDeal deal;
    deal.board = {card("As"), card("Ks"), card("Qd"), card("Jc"), card("Th")};  // Broadway plays
    deal.holes[0] = std::array<Card, 2>{card("2c"), card("3d")};
    deal.holes[1] = std::array<Card, 2>{card("4h"), card("5c")};
    deal.holes[2] = std::array<Card, 2>{card("6h"), card("7c")};
    e.start_hand(deal);

    call(e, 0);
    fold(e, 1);
    check(e, 2);
    for (int street = 0; street < 3; ++street) {
        check(e, 2); check(e, 0);
    }
    const GameState& s = e.state();
    EXPECT_EQ(s.seats[2].stack, 990 + 13);  // First clockwise from the button
    EXPECT_EQ(s.seats[0].stack, 990 + 12);
    EXPECT_EQ(s.seats[1].stack, 995);
}

TEST(GameEngine, SidePotSplitsWhileMainPotHasSingleWinner) {
    ScriptedDeal deal;
    deal.board = {card("2c"), card("7d"), card("9h"), card("Js"), card("3c")};
    deal.holes[0] = std::array<Card, 2>{card("As"), card("Ad")};  // Short stack, best hand
    deal.holes[1] = std::array<Card, 2>{card("Ks"), card("Kd")};  // Tied for the side pot
    deal.holes[2] = std::array<Card, 2>{card("Kh"), card("Kc")};

    GameEngine e(config_with({100, 500, 500, 0, 0, 0}, 0));
    e.start_hand(deal);
    all_in(e, 0);
    all_in(e, 1);
    call(e, 2);

    const GameState& s = e.state();
    EXPECT_EQ(s.seats[0].stack, 300);
    EXPECT_EQ(s.seats[1].stack, 400);
    EXPECT_EQ(s.seats[2].stack, 400);
}

// --- Freezeout ---------------------------------------------------------------

TEST(GameEngine, BustedBotStaysOutAndIsRecorded) {
    GameEngine e(config_with({100, 1000, 1000, 0, 0, 0}, 0));
    e.start_hand(strength_order_deal({5, 0, 1, 2, 3, 4}));
    all_in(e, 0);
    fold(e, 1);
    call(e, 2);

    const GameState& s = e.state();
    EXPECT_EQ(s.seats[0].status, SeatStatus::Busted);
    EXPECT_EQ(s.seats[0].stack, 0);
    EXPECT_EQ(s.seats[0].busted_on_hand, 1);
    EXPECT_EQ(s.phase, GamePhase::BetweenHands);

    e.start_hand();
    EXPECT_FALSE(s.seats[0].hole);
    EXPECT_EQ(s.seats[0].status, SeatStatus::Busted);
}

TEST(GameEngine, SessionEndsWhenHeroBusts) {
    GameConfig config = config_with({100, 1000, 1000, 0, 0, 0}, 0);
    config.hero_seat = 0;
    GameEngine e(config);
    e.start_hand(strength_order_deal({5, 0, 1, 2, 3, 4}));
    all_in(e, 0);
    call(e, 1);
    fold(e, 2);

    EXPECT_EQ(e.state().phase, GamePhase::SessionOver);
    EXPECT_THROW(e.start_hand(), std::logic_error);

    const SessionSummary summary = e.session_summary();
    EXPECT_TRUE(summary.hero_busted);
    EXPECT_FALSE(summary.winner);
    EXPECT_EQ(summary.hands_played, 1);
    EXPECT_EQ(summary.busted_on_hand[0], 1);
}

TEST(GameEngine, SessionEndsWhenHeroBustsAllBots) {
    GameConfig config = config_with({1000, 100, 100, 0, 0, 0}, 0);
    config.hero_seat = 0;
    GameEngine e(config);
    e.start_hand(strength_order_deal({0, 1, 2, 3, 4, 5}));
    raise_to(e, 0, 100);
    all_in(e, 1);
    all_in(e, 2);

    EXPECT_EQ(e.state().phase, GamePhase::SessionOver);
    const SessionSummary summary = e.session_summary();
    EXPECT_FALSE(summary.hero_busted);
    EXPECT_EQ(summary.winner, 0);
    EXPECT_EQ(summary.final_stacks[0], 1200);
}

TEST(GameEngine, NewGameResetsSeatsAndStacks) {
    GameConfig config = config_with({100, 1000, 1000, 0, 0, 0}, 0);
    config.hero_seat = 0;
    GameEngine e(config);
    e.start_hand(strength_order_deal({5, 0, 1, 2, 3, 4}));
    all_in(e, 0);
    call(e, 1);
    fold(e, 2);
    ASSERT_EQ(e.state().phase, GamePhase::SessionOver);

    e.new_game();
    const GameState& s = e.state();
    EXPECT_EQ(s.phase, GamePhase::BetweenHands);
    EXPECT_EQ(s.hand_number, 0);
    EXPECT_EQ(s.seats[0].stack, 100);
    EXPECT_EQ(s.seats[0].status, SeatStatus::Active);
    EXPECT_FALSE(s.seats[0].busted_on_hand);
    EXPECT_EQ(s.seats[3].status, SeatStatus::Busted);
    EXPECT_NO_THROW(e.start_hand());
}

TEST(GameEngine, ActionLogRecordsVoluntaryActions) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    raise_to(e, 0, 60);
    fold(e, 1);
    call(e, 2);
    bet(e, 2, 100);
    fold(e, 0);

    const auto& actions = e.state().actions;
    ASSERT_EQ(actions.size(), 5u);
    EXPECT_EQ(actions[0].type, ActionType::Raise);
    EXPECT_EQ(actions[0].chips_added, 60);
    EXPECT_EQ(actions[0].street, Street::Preflop);
    EXPECT_EQ(actions[2].type, ActionType::Call);
    EXPECT_EQ(actions[2].chips_added, 40);
    EXPECT_EQ(actions[3].street, Street::Flop);
    EXPECT_EQ(actions[3].street_total, 100);
}

TEST(GameEngine, ChipsAreConservedAcrossRandomizedSession) {
    GameConfig config;
    config.seed = 5;
    config.hero_seat = std::nullopt;
    GameEngine e(config);
    std::mt19937_64 rng(11);

    for (int hand = 0; hand < 500 && e.state().phase == GamePhase::BetweenHands; ++hand) {
        e.start_hand();
        while (e.state().phase == GamePhase::Betting) {
            const LegalActions legal = e.legal_actions();
            Action action{ActionType::Fold};
            if (legal.allows(ActionType::AllIn) && rng() % 8 == 0) action = {ActionType::AllIn};
            else if (legal.allows(ActionType::Raise) && rng() % 4 == 0) action = {ActionType::Raise, legal.min_raise_to};
            else if (legal.allows(ActionType::Bet) && rng() % 4 == 0) action = {ActionType::Bet, legal.min_raise_to};
            else if (legal.allows(ActionType::Check)) action = {ActionType::Check};
            else if (legal.allows(ActionType::Call)) action = {ActionType::Call};
            ASSERT_TRUE(e.apply_action(action));
        }
        ASSERT_EQ(stack_total(e), 12000);
    }
}

// --- Blind levels and antes ----------------------------------------------------

namespace {

GameConfig with_level(GameConfig config, int sb, int bb, int ante, AnteType type) {
    config.small_blind = sb;
    config.big_blind = bb;
    config.ante = ante;
    config.ante_type = type;
    return config;
}

// Checks whoever is to act until the hand ends.
void check_down(GameEngine& e) {
    while (e.state().phase == GamePhase::Betting) {
        ASSERT_TRUE(e.apply_action({ActionType::Check}));
    }
}

int chips_in_play(const GameEngine& e) {
    int total = 0;
    for (const Seat& s : e.state().seats) total += s.stack + s.total_bet + s.ante_posted;
    return total;
}

std::array<std::array<Card, 2>, kMaxSeats> holes_of(const GameEngine& e) {
    std::array<std::array<Card, 2>, kMaxSeats> holes{};
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (e.state().seats[seat].hole) holes[seat] = *e.state().seats[seat].hole;
    }
    return holes;
}

}  // namespace

TEST(GameEngineLevels, RejectsBadLevels) {
    GameEngine e(config_with(kThreeHanded));
    EXPECT_THROW(e.set_level({0, 20, 0, AnteType::None}), std::invalid_argument);
    EXPECT_THROW(e.set_level({30, 20, 0, AnteType::None}), std::invalid_argument);
    EXPECT_THROW(e.set_level({10, 20, -1, AnteType::PerPlayer}), std::invalid_argument);
    EXPECT_THROW(GameEngine(with_level(config_with(kThreeHanded), 10, 20, -5, AnteType::BigBlind)),
                 std::invalid_argument);
}

TEST(GameEngineLevels, NewLevelAppliesFromTheNextHandOnly) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    EXPECT_THROW(e.set_level({25, 50, 0, AnteType::None}), std::logic_error);
    fold(e, 0);
    fold(e, 1);

    e.set_level({25, 50, 0, AnteType::None});
    EXPECT_EQ(e.state().level.big_blind, 20);  // The finished hand keeps its blinds
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.level.small_blind, 25);
    EXPECT_EQ(s.level.big_blind, 50);
    EXPECT_EQ(s.small_blind_posted, 25);
    EXPECT_EQ(s.big_blind_posted, 50);
    EXPECT_EQ(s.current_bet, 50);
    EXPECT_EQ(e.legal_actions().min_raise_to, 100);
    EXPECT_EQ(e.player_view(0).big_blind, 50);
    EXPECT_EQ(e.player_view(0).small_blind, 25);
}

TEST(GameEngineLevels, MinimumBetPostflopIsTheHandsBigBlind) {
    GameEngine e(config_with(kThreeHanded));
    e.set_level({50, 100, 0, AnteType::None});
    e.start_hand();
    call(e, 0);
    call(e, 1);
    check(e, 2);
    EXPECT_EQ(e.legal_actions().min_raise_to, 100);
}

TEST(GameEngineLevels, NewGameWithConfigKeepsTheRandomStream) {
    GameConfig config = config_with(kThreeHanded);
    config.seed = 99;
    config.first_button.reset();

    GameEngine changed(config);
    changed.start_hand();
    changed.new_game(with_level(config, 50, 100, 10, AnteType::PerPlayer));
    changed.start_hand();

    GameEngine same(config);
    same.start_hand();
    same.new_game();
    same.start_hand();

    EXPECT_EQ(holes_of(changed), holes_of(same));
    EXPECT_EQ(changed.state().button, same.state().button);
    EXPECT_EQ(changed.state().level.big_blind, 100);

    GameEngine fresh(config);
    fresh.start_hand();
    EXPECT_NE(holes_of(fresh), holes_of(changed));  // No replay of the first deal
}

TEST(GameEngineLevels, NewGameWithABadConfigChangesNothing) {
    GameEngine e(config_with(kThreeHanded));
    GameConfig bad = config_with(kThreeHanded);
    bad.big_blind = 0;
    EXPECT_THROW(e.new_game(bad), std::invalid_argument);
    EXPECT_EQ(e.next_level().big_blind, 20);
    EXPECT_NO_THROW(e.start_hand());
}

TEST(GameEngineAntes, PerPlayerAntesWalkKeepsAntesAndRefundsOnlyTheBlindDifference) {
    GameEngine e(with_level(config_with(kThreeHanded), 10, 20, 5, AnteType::PerPlayer));
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.seats[0].stack, 995);
    EXPECT_EQ(s.seats[1].stack, 985);
    EXPECT_EQ(s.seats[2].stack, 975);
    EXPECT_EQ(s.seats[1].ante_posted, 5);
    EXPECT_EQ(s.seats[1].total_bet, 10);  // Antes are not bets
    EXPECT_EQ(s.total_pot(), 45);
    EXPECT_EQ(s.street_start_pot[0], 45);
    EXPECT_EQ(s.current_bet, 20);
    EXPECT_EQ(e.legal_actions().call_amount, 20);
    EXPECT_EQ(e.legal_actions().min_raise_to, 40);

    fold(e, 0);
    fold(e, 1);
    ASSERT_TRUE(s.last_result);
    ASSERT_EQ(s.last_result->payouts.size(), 1u);
    EXPECT_EQ(s.last_result->payouts[0].amount, 35);  // 3 antes + both blinds' 10
    ASSERT_TRUE(s.last_result->refund);
    EXPECT_EQ(s.last_result->refund->amount, 10);
    EXPECT_EQ(s.seats[2].stack, 1020);
    EXPECT_EQ(stack_total(e), 3000);
}

TEST(GameEngineAntes, BigBlindAnteIsDeadMoneyAfterTheBlind) {
    GameEngine e(with_level(config_with(kThreeHanded), 10, 20, 20, AnteType::BigBlind));
    e.start_hand();
    const GameState& s = e.state();
    EXPECT_EQ(s.seats[0].ante_posted, 0);
    EXPECT_EQ(s.seats[2].ante_posted, 20);
    EXPECT_EQ(s.seats[2].total_bet, 20);
    EXPECT_EQ(s.seats[2].stack, 960);
    EXPECT_EQ(s.total_pot(), 50);
    const LivePots live = e.live_pots();
    EXPECT_EQ(live.total, 50);
    EXPECT_EQ(live.uncalled, 10);
    ASSERT_EQ(live.pots.size(), 1u);
    EXPECT_EQ(live.pots[0].amount, 40);

    fold(e, 0);
    fold(e, 1);
    // The ante is never "uncalled": the BB gets it back only by winning the pot.
    EXPECT_EQ(s.last_result->payouts[0].amount, 40);
    EXPECT_EQ(s.last_result->refund->amount, 10);
    EXPECT_EQ(s.seats[2].stack, 1010);
    EXPECT_EQ(stack_total(e), 3000);
}

TEST(GameEngineAntes, ShortBigBlindPostsTheBlindBeforeTheAnte) {
    GameEngine e(with_level(config_with({1000, 1000, 25, 0, 0, 0}), 10, 20, 20, AnteType::BigBlind));
    e.start_hand(strength_order_deal({1, 2, 0, 3, 4, 5}));
    const GameState& s = e.state();
    EXPECT_EQ(s.big_blind_posted, 20);
    EXPECT_EQ(s.seats[2].ante_posted, 5);
    EXPECT_EQ(s.seats[2].status, SeatStatus::AllIn);
    EXPECT_EQ(s.current_bet, 20);

    call(e, 0);
    fold(e, 1);
    ASSERT_EQ(s.phase, GamePhase::BetweenHands);
    ASSERT_TRUE(s.last_result->showdown);
    // Main pot: 10 + 10 + 10 from the SB's blind, the rest of both 20s, plus 5 dead.
    ASSERT_EQ(s.last_result->pots.size(), 1u);
    EXPECT_EQ(s.last_result->pots[0].amount, 55);
    EXPECT_EQ(s.seats[2].stack, 55);
    EXPECT_EQ(stack_total(e), 2025);
}

TEST(GameEngineAntes, PlayerAllInForLessThanTheAnteGetsASidePotOfTheRightSize) {
    GameEngine e(with_level(config_with({1000, 1000, 1000, 3, 0, 0}), 10, 20, 5, AnteType::PerPlayer));
    e.start_hand(strength_order_deal({1, 2, 3, 0, 4, 5}));
    const GameState& s = e.state();
    EXPECT_EQ(s.seats[3].ante_posted, 3);
    EXPECT_EQ(s.seats[3].status, SeatStatus::AllIn);
    EXPECT_EQ(s.to_act, 0);  // UTG is all in from the ante

    call(e, 0);
    call(e, 1);
    check(e, 2);
    check_down(e);
    ASSERT_TRUE(s.last_result->showdown);
    ASSERT_EQ(s.last_result->pots.size(), 2u);
    EXPECT_EQ(s.last_result->pots[0].amount, 12);  // 3 from each of four players
    EXPECT_EQ(s.last_result->pots[1].amount, 66);  // 22 more from each of three
    EXPECT_EQ(s.seats[3].stack, 12);
    EXPECT_EQ(s.seats[0].stack, 1041);
    EXPECT_EQ(stack_total(e), 3003);
}

TEST(GameEngineAntes, SmallBlindAllInFromTheAntePostsNothing) {
    GameEngine e(with_level(config_with({1000, 4, 1000, 0, 0, 0}), 10, 20, 5, AnteType::PerPlayer));
    e.start_hand(strength_order_deal({1, 0, 2, 3, 4, 5}));
    const GameState& s = e.state();
    EXPECT_EQ(s.small_blind_seat, 1);
    EXPECT_EQ(s.small_blind_posted, 0);
    EXPECT_EQ(s.seats[1].ante_posted, 4);
    call(e, 0);
    check(e, 2);
    check_down(e);
    // Seat 1 wins 4 from each of the three; the rest goes to the best of seats 0 and 2.
    EXPECT_EQ(s.seats[1].stack, 12);
    EXPECT_EQ(stack_total(e), 2004);
}

TEST(GameEngineAntes, HeadsUpWithEitherAnte) {
    {
        GameEngine e(with_level(config_with({1000, 1000, 0, 0, 0, 0}), 10, 20, 5, AnteType::PerPlayer));
        e.start_hand();
        EXPECT_EQ(e.state().small_blind_seat, 0);
        EXPECT_EQ(e.state().total_pot(), 40);
        fold(e, 0);
        EXPECT_EQ(e.state().seats[1].stack, 1015);
        EXPECT_EQ(e.state().seats[0].stack, 985);
    }
    {
        GameEngine e(with_level(config_with({1000, 1000, 0, 0, 0, 0}), 10, 20, 20, AnteType::BigBlind));
        e.start_hand();
        EXPECT_EQ(e.state().total_pot(), 50);
        fold(e, 0);
        EXPECT_EQ(e.state().seats[1].stack, 1010);
        EXPECT_EQ(e.state().seats[0].stack, 990);
    }
}

TEST(GameEngineAntes, AntesSitOutsideBetsInTheView) {
    GameEngine e(with_level(config_with(kThreeHanded), 10, 20, 5, AnteType::PerPlayer));
    e.start_hand();
    const PlayerView view = e.player_view(0);
    EXPECT_EQ(view.ante, 5);
    EXPECT_EQ(view.ante_type, AnteType::PerPlayer);
    EXPECT_EQ(view.total_pot, 45);
    EXPECT_EQ(view.dead_money(), 15);
    EXPECT_EQ(view.seats[2].total_bet, 20);
    EXPECT_EQ(view.seats[2].ante_posted, 5);

    // Mid-hand, a summary counts chips already in the middle as the owner's.
    const SessionSummary summary = e.session_summary();
    EXPECT_EQ(summary.final_stacks[0], 1000);
    EXPECT_EQ(summary.final_stacks[2], 1000);
}

TEST(GameEngineAntes, NoAnteMeansNoDeadMoney) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    EXPECT_EQ(e.player_view(0).dead_money(), 0);
    EXPECT_EQ(e.player_view(0).ante_type, AnteType::None);
    EXPECT_EQ(e.state().total_antes(), 0);
}

TEST(GameEngineAntes, ChipsAreConservedThroughEveryActionWithRisingLevelsAndAntes) {
    for (const AnteType type : {AnteType::PerPlayer, AnteType::BigBlind}) {
        GameConfig config;
        config.seed = 21;
        config.hero_seat = std::nullopt;
        config.starting_stack = 600;
        GameEngine e(config);
        std::mt19937_64 rng(13);

        int hand = 0;
        int level_hand = 0;
        for (; hand < 400; ++hand, ++level_hand) {
            if (e.state().phase == GamePhase::SessionOver) {
                e.new_game();
                level_hand = 0;
            }
            const int bb = 20 + 20 * (level_hand / 15);
            e.set_level({bb / 2, bb, type == AnteType::BigBlind ? bb : bb / 8, type});
            e.start_hand();  // Can run out at once when blinds and antes put players all in
            if (e.state().phase == GamePhase::Betting) {
                ASSERT_EQ(chips_in_play(e), 3600);
            }
            while (e.state().phase == GamePhase::Betting) {
                const LegalActions legal = e.legal_actions();
                Action action{ActionType::Fold};
                if (legal.allows(ActionType::AllIn) && rng() % 6 == 0) action = {ActionType::AllIn};
                else if (legal.allows(ActionType::Raise) && rng() % 4 == 0) action = {ActionType::Raise, legal.min_raise_to};
                else if (legal.allows(ActionType::Bet) && rng() % 4 == 0) action = {ActionType::Bet, legal.min_raise_to};
                else if (legal.allows(ActionType::Check)) action = {ActionType::Check};
                else if (legal.allows(ActionType::Call) && rng() % 3 != 0) action = {ActionType::Call};
                ASSERT_TRUE(e.apply_action(action));
                if (e.state().phase != GamePhase::Betting) break;  // Paid out; bets stay recorded
                ASSERT_EQ(chips_in_play(e), 3600) << to_string(type) << " hand " << hand;
                const LivePots live = e.live_pots();
                int shown = live.uncalled;
                for (const Pot& pot : live.pots) shown += pot.amount;
                ASSERT_EQ(shown, live.total);
                ASSERT_EQ(live.total, e.state().total_pot());
            }
            ASSERT_EQ(stack_total(e), 3600) << to_string(type) << " hand " << hand;
        }
        EXPECT_EQ(hand, 400);
    }
}
