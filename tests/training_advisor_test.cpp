#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "advisor/training_advisor.h"
#include "bots/gto_heuristic_engine.h"
#include "engine/game_engine.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

static_assert(std::is_invocable_v<decltype(&TrainingAdvisor::analyze), const TrainingAdvisor&, const PlayerView&>);
static_assert(!std::is_invocable_v<decltype(&TrainingAdvisor::analyze), const TrainingAdvisor&, const GameState&>);

constexpr int kRollouts = 2000;

Card card(const char* text) { return Card::parse(text).value(); }
std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }
std::array<Card, 5> board_of(const char* a, const char* b, const char* c, const char* d, const char* e) {
    return {card(a), card(b), card(c), card(d), card(e)};
}

GameEngine heads_up_to_flop(const std::array<Card, 2>& seat0, const std::array<Card, 2>& seat1,
                            const std::array<Card, 5>& board) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = seat0;
    deal.holes[1] = seat1;
    deal.board = board;
    engine.start_hand(deal);
    EXPECT_TRUE(engine.apply_action({ActionType::Raise, 100}));
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    return engine;
}

GameEngine six_max(int seat, const std::array<Card, 2>& cards, std::uint64_t seed = 5) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.seed = seed;
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[seat] = cards;
    deal.board = board_of("2c", "7d", "Jh", "4s", "9c");
    std::array<bool, kDeckSize> used{};
    for (const Card c : deal.board) used[c.id] = true;
    used[cards[0].id] = true;
    used[cards[1].id] = true;
    int next = 0;
    const auto spare = [&] {
        while (used[static_cast<std::size_t>(next)]) ++next;
        used[static_cast<std::size_t>(next)] = true;
        return Card(next);
    };
    for (int s = 0; s < kMaxSeats; ++s) {
        if (!deal.holes[s]) deal.holes[s] = std::array<Card, 2>{spare(), spare()};
    }
    engine.start_hand(deal);
    return engine;
}

// BTN to act behind three limpers.
GameEngine six_max_limped(const std::array<Card, 2>& cards) {
    GameEngine engine = six_max(0, cards);
    for (int limp = 0; limp < 3; ++limp) EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    return engine;
}

PreflopResponse chart_response_for_shove(const char* text) {
    const HandClass hand = HandClass::parse(text).value();
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            if (HandClass::from_cards(Card(a), Card(b)) != hand || a == card("2c").id || b == card("2c").id ||
                a == card("7d").id || b == card("7d").id || a == card("Jh").id || b == card("Jh").id ||
                a == card("4s").id || b == card("4s").id || a == card("9c").id || b == card("9c").id) {
                continue;
            }
            GameEngine engine = six_max(2, {Card(a), Card(b)});
            while (engine.state().to_act != 3) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
            while (engine.state().to_act != 2) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            return chart_response(engine.player_view(2));
        }
    }
    throw std::logic_error("no cards for hand");
}

bool is_aggressive(ActionType type) {
    return type == ActionType::Bet || type == ActionType::Raise || type == ActionType::AllIn;
}

bool is_legal(const LegalActions& legal, const Action& a) {
    if (!legal.allows(a.type)) return false;
    if (a.type == ActionType::Bet || a.type == ActionType::Raise) {
        return a.amount >= legal.min_raise_to && a.amount <= legal.max_raise_to;
    }
    return true;
}

double best_listed_ev(const EvEstimates& ev) {
    double best = -1e18;
    for (const auto& option : {ev.fold, ev.check_call, ev.raise_33, ev.raise_75, ev.all_in}) {
        if (option) best = std::max(best, *option);
    }
    return best;
}

double ev_of_action(const HintRecommendation& hint) {
    switch (hint.action.type) {
        case ActionType::Fold: return hint.ev.fold.value();
        case ActionType::Check:
        case ActionType::Call: return hint.ev.check_call.value();
        case ActionType::AllIn: return hint.ev.all_in.value();
        case ActionType::Bet:
        case ActionType::Raise:
            if (hint.raise_33_to == hint.action.amount) return hint.ev.raise_33.value();
            return hint.ev.raise_75.value();
    }
    return 0.0;
}

TEST(Advisor, PotOddsAndMdfAreExactInAKnownSpot) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));  // 150 into 200
    const HintRecommendation hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(0));
    EXPECT_DOUBLE_EQ(hint.pot_odds_pct, 100.0 * 150.0 / 500.0);
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0 * 200.0 / 350.0);
    EXPECT_DOUBLE_EQ(hint.spr, 1750.0 / 500.0);  // Each has 1750 behind and 500 is in the pot after calling
}

TEST(Advisor, NothingToCallMeansNoOddsAndFullDefense) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(1));
    EXPECT_DOUBLE_EQ(hint.pot_odds_pct, 0.0);
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0);
    EXPECT_DOUBLE_EQ(hint.spr, 1900.0 / 200.0);
}

TEST(Advisor, EvFormulasMatchTheDocumentedModelForCheckAndCall) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation check_hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(1));
    EXPECT_NEAR(*check_hint.ev.check_call, check_hint.equity_pct / 100.0 * 200.0, 1e-9);

    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));
    const HintRecommendation call_hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(0));
    EXPECT_EQ(*call_hint.ev.fold, 0.0);
    // 150 into 200 is 0.75 of a pot, at the bots' discount of 0.10 per pot.
    EXPECT_NEAR(*call_hint.ev.check_call, (call_hint.equity_pct / 100.0 - 0.075) * 500.0 - 150.0, 1e-9);
}

// The documented raise formula, by hand: heads-up nobody has bet, so the fold model
// uses the MDF for the size with a 10% call floor, and the exponent is 1 + f + o.
double unfaced_raise_ev(double equity, double pot, double added, double behind_after) {
    const double s = added / pot;
    const double call = std::max(1.0 / (1.0 + s), 0.10);
    const double fold = 1.0 - call;
    const double called = std::pow(equity, 1.0 + fold + 0.25 * std::max(0.0, s - 1.0));
    const double implied = std::max(0.0, called - 0.5) * behind_after * 0.5;
    return fold * pot + call * (called * (pot + added + added) - added + implied);
}

TEST(Advisor, RaiseEvsMatchTheDocumentedModel) {
    // A set on a rainbow flop, checked to. Pot 200, 1900 behind each.
    GameEngine engine = heads_up_to_flop(hole("Ac", "Ah"), hole("8c", "8d"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(20, kRollouts).analyze(engine.player_view(1));
    const double e = hint.equity_pct / 100.0;
    ASSERT_EQ(hint.raise_33_to, 66);
    ASSERT_EQ(hint.raise_75_to, 150);
    EXPECT_NEAR(*hint.ev.raise_33, unfaced_raise_ev(e, 200.0, 66.0, 1900.0 - 66.0), 1e-9);
    EXPECT_NEAR(*hint.ev.raise_75, unfaced_raise_ev(e, 200.0, 150.0, 1900.0 - 150.0), 1e-9);
    EXPECT_NEAR(*hint.ev.all_in, unfaced_raise_ev(e, 200.0, 1900.0, 0.0), 1e-9);
}

TEST(Advisor, RiverNutsRaiseEvsAreExact) {
    // Equity 1 and no implied odds leave only fold equity and the size.
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    const HintRecommendation hint = TrainingAdvisor(21, kRollouts).analyze(engine.player_view(1));
    ASSERT_DOUBLE_EQ(hint.equity_pct, 100.0);
    EXPECT_NEAR(*hint.ev.all_in, 0.9 * 200.0 + 0.1 * (200.0 + 1900.0), 1e-9);  // The 10% call floor
    const double call_75 = 1.0 / 1.75;
    EXPECT_NEAR(*hint.ev.raise_75, (1.0 - call_75) * 200.0 + call_75 * (200.0 + 150.0), 1e-9);
}

TEST(Advisor, DifferentDecisionsGetDifferentRollouts) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const PlayerView view = engine.player_view(3);
    const TrainingAdvisor advisor(22, kRollouts);
    const double base = advisor.analyze(view).equity_pct;

    PlayerView next_hand = view;
    ++next_hand.hand_number;
    EXPECT_NE(advisor.analyze(next_hand).equity_pct, base);

    PlayerView later_in_hand = view;
    later_in_hand.actions.push_back(ActionRecord{Street::Preflop, 4, ActionType::Fold, 0, 0, false});
    EXPECT_NE(advisor.analyze(later_in_hand).equity_pct, base);
}

// All chips above the short all-in are heads-up, so only that much is three-way.
TEST(Advisor, CallEvCountsEachPotAgainstItsOwnOpponents) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 40, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    const std::array<Card, 2> mine = hole("Qc", "5d");
    const std::array<Card, 5> board = board_of("Ks", "7c", "2d", "9h", "4s");
    deal.holes[0] = mine;
    deal.holes[1] = hole("Ac", "Ad");
    deal.holes[2] = hole("Kc", "Kd");
    deal.board = board;
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 1000}));

    EquityOptions options;
    options.rollouts = 200000;
    options.seed = 1;
    const double one = equity_vs_random(mine, board, 1, options).equity;
    const double two = equity_vs_random(mine, board, 2, options).equity;
    const double expected = ((120.0 * two + 2000.0 * one) / 2120.0 - 0.15) * 2120.0 - 1000.0;

    constexpr int kAdvisorRollouts = 50000;
    const HintRecommendation hint = TrainingAdvisor(23, kAdvisorRollouts).analyze(engine.player_view(0));
    EXPECT_NEAR(*hint.ev.check_call, expected, 15.0);  // About 4 standard errors on 2000 chips
    EXPECT_EQ(hint.action.type, ActionType::Fold);
}

// A hand in every class, avoiding six_max's fixed board.
std::array<Card, 2> cards_for(HandClass hand) {
    const std::array<Card, 5> board = board_of("2c", "7d", "Jh", "4s", "9c");
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = 0; b < kDeckSize; ++b) {
            const Card x(a);
            const Card y(b);
            if (a == b || std::find(board.begin(), board.end(), x) != board.end() ||
                std::find(board.begin(), board.end(), y) != board.end()) {
                continue;
            }
            if (HandClass::from_cards(x, y) == hand) return {x, y};
        }
    }
    throw std::logic_error("no cards for hand");
}

TEST(Advisor, TrashBehindLimpersDoesNotRaise) {
    int raises = 0;
    const TrainingAdvisor advisor(24, 1000);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(0, cards_for(hand));
        for (int limp = 0; limp < 3; ++limp) ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_EQ(engine.state().to_act, 0);
        const HintRecommendation hint = advisor.analyze(engine.player_view(0));
        if (is_aggressive(hint.action.type)) {
            ++raises;
            EXPECT_TRUE(should_open(hand, Position::BTN)) << hand.to_string() << ": " << hint.best_action;
        }
    }
    EXPECT_LT(raises, 100);  // It used to be all 169

    const HintRecommendation trash = advisor.analyze(six_max_limped(hole("7c", "2d")).player_view(0));
    EXPECT_EQ(trash.action.type, ActionType::Fold) << trash.best_action;
    const HintRecommendation aces = advisor.analyze(six_max_limped(hole("Ac", "Ad")).player_view(0));
    EXPECT_TRUE(is_aggressive(aces.action.type)) << aces.best_action;
}

TEST(Advisor, FacingAShoveAgreesWithTheChartEverywhere) {
    const TrainingAdvisor advisor(25, kRollouts);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(2, cards_for(hand));
        while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(2);
        const HintRecommendation hint = advisor.analyze(view);
        const bool chart_folds = chart_response(view) == PreflopResponse::Fold;
        EXPECT_EQ(hint.action.type, chart_folds ? ActionType::Fold : ActionType::Call)
            << hand.to_string() << ": " << hint.best_action << " " << hint.rationale;
    }
    for (const char* hand : {"Q9o", "K4o", "T8o", "55", "TT"}) {
        EXPECT_EQ(chart_response_for_shove(hand), PreflopResponse::Fold) << hand;
    }
    for (const char* hand : {"AA", "KK", "QQ", "AKo"}) {
        EXPECT_NE(chart_response_for_shove(hand), PreflopResponse::Fold) << hand;
    }
}

TEST(Advisor, FacingANormalOpenFollowsTheChartOnWhetherToPlay) {
    const TrainingAdvisor advisor(30, kRollouts);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(2, cards_for(hand));
        while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
        while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(2);
        const HintRecommendation hint = advisor.analyze(view);
        const bool chart_folds = chart_response(view) == PreflopResponse::Fold;
        EXPECT_EQ(hint.action.type == ActionType::Fold, chart_folds) << hand.to_string() << ": " << hint.best_action;
    }
}

TEST(Advisor, ACheapCallIsAdvisedWhereTheChartWouldFold) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 2000, 30, 2000, 2000};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[2] = hole("Jc", "6d");
    deal.board = board_of("2c", "7d", "Jh", "4s", "9c");
    deal.board[2] = card("Qh");  // The J is in use
    deal.holes[0] = hole("As", "Ad");
    deal.holes[1] = hole("Ks", "Kd");
    deal.holes[3] = hole("Qs", "Qd");
    deal.holes[4] = hole("Ts", "Td");
    deal.holes[5] = hole("8s", "8d");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(26, kRollouts).analyze(engine.player_view(2));
    EXPECT_EQ(hint.action.type, ActionType::Call) << hint.best_action;
}

TEST(Advisor, AFoldAboveThePriceExplainsItself) {
    // T5o against a minimum raise: the chart folds at 22.2%, under about 40% equity.
    GameEngine engine = six_max(2, hole("Tc", "5d"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 40}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(27, kRollouts).analyze(engine.player_view(2));
    ASSERT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
    ASSERT_GT(hint.equity_pct, hint.pot_odds_pct);
    EXPECT_NE(hint.rationale.find("chart"), std::string::npos) << hint.rationale;
    EXPECT_EQ(hint.rationale.find("highest estimated EV"), std::string::npos) << hint.rationale;
}

TEST(Advisor, AnUnopenedPotMentionsNoMdf) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const HintRecommendation hint = TrainingAdvisor(28, kRollouts).analyze(engine.player_view(3));
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0);
    EXPECT_EQ(hint.rationale.find("MDF"), std::string::npos) << hint.rationale;
}

TEST(Advisor, SizesNeverExceedWhatAnOpponentCanMatch) {
    // Aces with 1600 behind, checked to by someone with 100 behind, in a pot of 800.
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{500, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Qs", "Qd");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 400}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    const HintRecommendation hint = TrainingAdvisor(29, kRollouts).analyze(engine.player_view(1));
    EXPECT_EQ(hint.action.type, ActionType::Bet) << hint.best_action;
    EXPECT_EQ(hint.action.amount, 100);
    EXPECT_EQ(hint.best_action, "BET 100");
    EXPECT_EQ(hint.raise_33_to, 100);
    EXPECT_FALSE(hint.raise_75_to.has_value());  // The same bet as the 33% one, so listed once
}

TEST(Advisor, AcesRecommendAggressionPreflop) {
    // Not the big blind: folded to, it wins without a decision.
    for (const int seat : {3, 4, 5, 0, 1}) {
        GameEngine engine = six_max(seat, hole("Ac", "Ad"));
        while (engine.state().to_act != seat) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const HintRecommendation hint = TrainingAdvisor(2, kRollouts).analyze(engine.player_view(seat));
        EXPECT_TRUE(is_aggressive(hint.action.type)) << hint.best_action;
    }
}

TEST(Advisor, AcesFacingARaiseStayAggressive) {
    GameEngine engine = six_max(2, hole("Ac", "Ad"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(3, kRollouts).analyze(engine.player_view(2));
    EXPECT_TRUE(is_aggressive(hint.action.type)) << hint.best_action;
}

TEST(Advisor, TrashFacingABigRaiseFolds) {
    GameEngine engine = six_max(2, hole("7c", "2d"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 300}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(4, kRollouts).analyze(engine.player_view(2));
    EXPECT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
    EXPECT_EQ(hint.best_action, "FOLD");
}

TEST(Advisor, TrashFirstInFoldsFromEarlyPosition) {
    GameEngine engine = six_max(3, hole("7c", "2d"));
    const HintRecommendation hint = TrainingAdvisor(5, kRollouts).analyze(engine.player_view(3));
    EXPECT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
}

TEST(Advisor, RiverNutsBetOrRaise) {
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    const HintRecommendation checked_to = TrainingAdvisor(6, kRollouts).analyze(engine.player_view(1));
    EXPECT_DOUBLE_EQ(checked_to.equity_pct, 100.0);
    EXPECT_TRUE(is_aggressive(checked_to.action.type)) << checked_to.best_action;

    GameEngine facing = heads_up_to_flop(hole("Ah", "Kh"), hole("Tc", "Td"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
    }
    ASSERT_TRUE(facing.apply_action({ActionType::Bet, 100}));
    const HintRecommendation raised = TrainingAdvisor(6, kRollouts).analyze(facing.player_view(0));
    EXPECT_TRUE(is_aggressive(raised.action.type)) << raised.best_action;
}

TEST(Advisor, RationaleNamesPositionTierTextureAndNumbers) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));
    const HintRecommendation hint = TrainingAdvisor(7, kRollouts).analyze(engine.player_view(0));
    EXPECT_NE(hint.rationale.find("BTN"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("dry"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("tier"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("pot odds"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("30.0%"), std::string::npos) << hint.rationale;

    GameEngine pre = six_max(3, hole("Ac", "Kd"));
    const std::string text = TrainingAdvisor(7, kRollouts).analyze(pre.player_view(3)).rationale;
    EXPECT_NE(text.find("UTG"), std::string::npos) << text;
    EXPECT_NE(text.find("no board"), std::string::npos) << text;
    EXPECT_NE(text.find("premium"), std::string::npos) << text;
}

TEST(Advisor, IllegalOptionsAreAbsent) {
    // Facing an all-in that covers the stack: only fold or call (the all-in is the call).
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{1000, 200, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Ks", "9h");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    const HintRecommendation hint = TrainingAdvisor(8, kRollouts).analyze(engine.player_view(1));
    EXPECT_FALSE(hint.ev.raise_33.has_value());
    EXPECT_FALSE(hint.ev.raise_75.has_value());
    EXPECT_TRUE(hint.ev.check_call.has_value());
    EXPECT_EQ(hint.action.type, ActionType::Call) << hint.best_action;  // AA for a bargain
}

TEST(Advisor, RefusesASeatThatIsNotToAct) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    EXPECT_THROW((void)TrainingAdvisor(9, kRollouts).analyze(engine.player_view(0)), std::invalid_argument);
}

TEST(Advisor, SameViewGivesTheSameHint) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const TrainingAdvisor advisor(10, kRollouts);
    const PlayerView view = engine.player_view(3);
    const HintRecommendation a = advisor.analyze(view);
    const HintRecommendation b = advisor.analyze(view);
    EXPECT_EQ(a.equity_pct, b.equity_pct);
    EXPECT_EQ(a.best_action, b.best_action);
    EXPECT_EQ(a.rationale, b.rationale);
}

TEST(Advisor, HintsIgnoreOpponentCardsAndUndealtBoard) {
    const auto hint_for = [](const std::array<Card, 2>& opponent, const char* turn, const char* river) {
        GameEngine engine = heads_up_to_flop(opponent, hole("Qh", "Jh"), board_of("Th", "9c", "2h", turn, river));
        return TrainingAdvisor(11, kRollouts).analyze(engine.player_view(1));
    };
    const HintRecommendation a = hint_for(hole("As", "Ad"), "3d", "4d");
    const HintRecommendation b = hint_for(hole("7c", "2s"), "Kd", "Ks");
    EXPECT_EQ(a.equity_pct, b.equity_pct);
    EXPECT_EQ(a.best_action, b.best_action);
    EXPECT_EQ(a.rationale, b.rationale);
    EXPECT_EQ(a.ev.check_call, b.ev.check_call);
    EXPECT_EQ(a.ev.raise_33, b.ev.raise_33);
    EXPECT_EQ(a.ev.raise_75, b.ev.raise_75);
    EXPECT_EQ(a.ev.all_in, b.ev.all_in);
}

// Every field, in every state random play reaches.
TEST(Advisor, FieldsInRangeAndBestActionLegalAcrossRandomStates) {
    std::mt19937_64 rng(4242);
    const TrainingAdvisor advisor(12, 100);
    int checked = 0;
    for (int game = 0; game < 120; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        std::array<int, kMaxSeats> stacks{};
        for (int& stack : stacks) stack = rng() % 4 == 0 ? 1 + static_cast<int>(rng() % 60) : 100 + static_cast<int>(rng() % 3000);
        config.stacks = stacks;
        GameEngine engine(config);
        for (int hand = 0; hand < 6 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            while (engine.state().to_act) {
                const PlayerView view = engine.player_view(*engine.state().to_act);
                const HintRecommendation hint = advisor.analyze(view);
                ++checked;

                EXPECT_GE(hint.equity_pct, 0.0);
                EXPECT_LE(hint.equity_pct, 100.0);
                EXPECT_GE(hint.pot_odds_pct, 0.0);
                EXPECT_LE(hint.pot_odds_pct, 100.0);
                EXPECT_GE(hint.mdf_pct, 0.0);
                EXPECT_LE(hint.mdf_pct, 100.0);
                EXPECT_GE(hint.spr, 0.0);
                EXPECT_FALSE(hint.best_action.empty());
                EXPECT_FALSE(hint.rationale.empty());

                EXPECT_TRUE(is_legal(view.legal, hint.action)) << hint.best_action;
                if (hint.action.type == ActionType::Bet || hint.action.type == ActionType::Raise) {
                    EXPECT_EQ(hint.action.amount, clamp_raise_to(view, hint.action.amount)) << hint.best_action;
                }
                if (hint.action.type == ActionType::Fold && hint.equity_pct > hint.pot_odds_pct) {
                    EXPECT_NE(hint.rationale.find("overstates"), std::string::npos) << hint.rationale;
                }
                EXPECT_DOUBLE_EQ(ev_of_action(hint), best_listed_ev(hint.ev)) << hint.best_action;

                if (!view.legal.allows(ActionType::Bet) && !view.legal.allows(ActionType::Raise)) {
                    EXPECT_FALSE(hint.ev.raise_33.has_value());
                    EXPECT_FALSE(hint.ev.raise_75.has_value());
                }
                if (!view.legal.allows(ActionType::AllIn)) EXPECT_FALSE(hint.ev.all_in.has_value());

                const LegalActions& legal = view.legal;
                std::vector<Action> options;
                if (legal.allows(ActionType::Check)) options.push_back({ActionType::Check, 0});
                if (legal.allows(ActionType::Call)) options.push_back({ActionType::Call, 0});
                if (legal.allows(ActionType::Fold)) options.push_back({ActionType::Fold, 0});
                if (legal.allows(ActionType::AllIn)) options.push_back({ActionType::AllIn, 0});
                if (legal.allows(ActionType::Raise)) options.push_back({ActionType::Raise, legal.min_raise_to});
                if (legal.allows(ActionType::Bet)) options.push_back({ActionType::Bet, legal.min_raise_to});
                ASSERT_TRUE(engine.apply_action(options[static_cast<std::size_t>(rng() % options.size())]));
            }
        }
    }
    EXPECT_GT(checked, 200);
}

// The failure modes a single-street formula drifts into, which a trainer must not teach.
TEST(Advisor, PreflopAdviceAgreesWithTheChartsInClearCases) {
    const auto first_in = [](int seat, const std::array<Card, 2>& h) {
        GameEngine engine = six_max(seat, h);
        while (engine.state().to_act != seat) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        return TrainingAdvisor(14, kRollouts).analyze(engine.player_view(seat)).action.type;
    };
    EXPECT_TRUE(is_aggressive(first_in(3, hole("Ac", "Kd"))));  // Not a fold because five random hands out-draw it
    EXPECT_TRUE(is_aggressive(first_in(3, hole("Tc", "Td"))));
    EXPECT_EQ(first_in(3, hole("Kc", "Td")), ActionType::Fold);
    EXPECT_EQ(first_in(0, hole("7c", "2d")), ActionType::Fold);  // Not even a button steal
}

TEST(Advisor, DoesNotShoveAirOrTopPairDeep) {
    const auto board = board_of("Kd", "8s", "3c", "2h", "Jd");

    GameEngine air = heads_up_to_flop(hole("Ac", "Ah"), hole("5c", "4d"), board);
    EXPECT_EQ(TrainingAdvisor(15, kRollouts).analyze(air.player_view(1)).action.type, ActionType::Check);

    GameEngine air_facing = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ah"), board);
    ASSERT_TRUE(air_facing.apply_action({ActionType::Bet, 150}));
    EXPECT_EQ(TrainingAdvisor(15, kRollouts).analyze(air_facing.player_view(0)).action.type, ActionType::Fold);

    GameEngine top_pair = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ah"), board);
    ASSERT_TRUE(top_pair.apply_action({ActionType::Bet, 150}));
    EXPECT_NE(TrainingAdvisor(15, kRollouts).analyze(top_pair.player_view(0)).action.type, ActionType::AllIn);
}

// Run by hand: reads the advice for a spread of spots.
TEST(Advisor, DISABLED_PrintSampleHints) {
    const auto show = [](const char* what, const PlayerView& view) {
        const HintRecommendation h = TrainingAdvisor(1).analyze(view);
        std::printf("%s\n  -> %s\n  %s\n  EV fold %.0f call %.0f r33 %.0f r75 %.0f shove %.0f\n", what,
                    h.best_action.c_str(), h.rationale.c_str(), h.ev.fold.value_or(-1), h.ev.check_call.value_or(-1),
                    h.ev.raise_33.value_or(-1), h.ev.raise_75.value_or(-1), h.ev.all_in.value_or(-1));
    };
    for (const auto& h : {hole("Ac", "Ad"), hole("Ac", "Kd"), hole("Tc", "Td"), hole("Kc", "Td"), hole("8h", "7h"), hole("9h", "4d")}) {
        GameEngine utg = six_max(3, h);
        show("UTG first in", utg.player_view(3));
        GameEngine bb = six_max(2, h);
        while (bb.state().to_act != 3) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(bb.apply_action({ActionType::Raise, 60}));
        while (bb.state().to_act != 2) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        show("BB vs 3x UTG", bb.player_view(2));
    }
    const auto b = board_of("Kd", "8s", "3c", "2h", "Jd");
    for (const auto& h : {hole("Ks", "9h"), hole("8c", "8d"), hole("5c", "4d"), hole("Tc", "9c")}) {
        GameEngine checked = heads_up_to_flop(hole("Ac", "Ah"), h, b);
        show("flop, checked to", checked.player_view(1));
        GameEngine facing = heads_up_to_flop(h, hole("Ac", "Ah"), b);
        ASSERT_TRUE(facing.apply_action({ActionType::Bet, 150}));
        show("flop, facing 75%", facing.player_view(0));
    }
}

TEST(Advisor, DefaultRolloutsPath) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(13).analyze(engine.player_view(1));
    EXPECT_GT(hint.equity_pct, 0.0);
}

}  // namespace
