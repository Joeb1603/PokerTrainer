#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "bots/bots.h"
#include "engine/game_engine.h"
#include "equity/equity.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

// Compile-time half of the anti-cheat contract: decide takes a view and nothing else.
static_assert(std::is_invocable_v<decltype(&IBotStrategy::decide), IBotStrategy&, const PlayerView&>);
static_assert(!std::is_invocable_v<decltype(&IBotStrategy::decide), IBotStrategy&, const GameState&>);
static_assert(!std::is_invocable_v<decltype(&IBotStrategy::decide), IBotStrategy&, const GameEngine&>);

constexpr int kFastRollouts = 200;
constexpr int kScenarioRollouts = 2000;

Card card(const char* text) { return Card::parse(text).value(); }

std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }

// Spare hole cards for seats a test does not care about, avoiding the given cards.
ScriptedDeal fill_deal(ScriptedDeal deal, const std::vector<int>& live_seats) {
    std::array<bool, kDeckSize> used{};
    for (const Card c : deal.board) used[c.id] = true;
    for (const auto& h : deal.holes) {
        if (h) {
            used[(*h)[0].id] = true;
            used[(*h)[1].id] = true;
        }
    }
    int next = 0;
    const auto spare = [&] {
        while (used[static_cast<std::size_t>(next)]) ++next;
        used[static_cast<std::size_t>(next)] = true;
        return Card(next);
    };
    for (const int seat : live_seats) {
        if (!deal.holes[seat]) deal.holes[seat] = std::array<Card, 2>{spare(), spare()};
    }
    return deal;
}

std::array<Card, 5> board_of(const char* a, const char* b, const char* c, const char* d, const char* e) {
    return {card(a), card(b), card(c), card(d), card(e)};
}

// Heads-up: seat 0 is button and small blind, seat 1 big blind. Preflop, the
// button raises to `preflop_to` (or limps if 0) and the big blind calls, so seat
// 1 acts first on the flop.
GameEngine heads_up_to_flop(const std::array<Card, 2>& seat0, const std::array<Card, 2>& seat1,
                            const std::array<Card, 5>& board, int preflop_to = 100, int stack = 2000) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{stack, stack, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = seat0;
    deal.holes[1] = seat1;
    deal.board = board;
    engine.start_hand(deal);
    if (preflop_to > 0) {
        EXPECT_TRUE(engine.apply_action({ActionType::Raise, preflop_to}));
        EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    } else {
        EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    EXPECT_EQ(engine.state().street, Street::Flop);
    return engine;
}

void check_street(GameEngine& engine) {
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
}

// Six-handed, button on seat 0, so seat 3 is UTG, 4 HJ, 5 CO, 0 BTN, 1 SB, 2 BB.
GameEngine six_max(int seat, const std::array<Card, 2>& cards) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[seat] = cards;
    deal.board = board_of("2c", "7d", "Jh", "4s", "9c");
    if (cards[0] == deal.board[0] || cards[1] == deal.board[0]) deal.board[0] = card("3c");
    engine.start_hand(fill_deal(deal, {0, 1, 2, 3, 4, 5}));
    return engine;
}

void fold_until(GameEngine& engine, int seat) {
    while (engine.state().to_act != seat) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
}

bool is_aggressive(const Action& a) {
    return a.type == ActionType::Bet || a.type == ActionType::Raise || a.type == ActionType::AllIn;
}

bool is_legal(const LegalActions& legal, const Action& a) {
    if (!legal.allows(a.type)) return false;
    if (a.type == ActionType::Bet || a.type == ActionType::Raise) {
        return a.amount >= legal.min_raise_to && a.amount <= legal.max_raise_to;
    }
    return true;
}

bool same(const Action& a, const Action& b) {
    if (a.type != b.type) return false;
    return (a.type != ActionType::Bet && a.type != ActionType::Raise) || a.amount == b.amount;
}

// Tolerance for a binomial proportion, wide enough for a fixed-seed test not to be
// a coin flip, narrow enough to catch a wrong rate.
double tolerance(double p, int n, double sigmas = 4.0) { return sigmas * std::sqrt(p * (1.0 - p) / n); }

double equity_of(const std::array<Card, 2>& h, std::span<const Card> board, int rollouts = 20000, int opponents = 1) {
    EquityOptions options;
    options.rollouts = rollouts;
    options.seed = 12345;
    return equity_vs_random(h, board, opponents, options).equity;
}

// Six-handed with chosen stacks, button on seat 0: seat 3 UTG, seat 2 BB.
GameEngine six_max_stacks(const std::array<int, kMaxSeats>& stacks, int seat, const std::array<Card, 2>& cards) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = stacks;
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[seat] = cards;
    deal.board = board_of("Kh", "Qh", "Jh", "9h", "3h");
    engine.start_hand(fill_deal(deal, {0, 1, 2, 3, 4, 5}));
    return engine;
}

std::array<Card, 2> cards_of(const char* text) {
    const HandClass hand = HandClass::parse(text).value();
    const auto pick = [](Rank rank, Suit suit) {
        return Card(static_cast<int>(rank) * kSuitCount + static_cast<int>(suit));
    };
    if (hand.suited) return {pick(hand.high, Suit::Clubs), pick(hand.low, Suit::Clubs)};
    return {pick(hand.high, Suit::Clubs), pick(hand.low, Suit::Diamonds)};
}

// Rates of each answer to one view, for the frequency tests.
struct Answers {
    double calls = 0.0;
    double raises = 0.0;
    double folds = 0.0;
};

Answers answer_rates(IBotStrategy& bot, const PlayerView& view, int decisions) {
    int calls = 0;
    int raises = 0;
    int folds = 0;
    for (int i = 0; i < decisions; ++i) {
        const Action action = bot.decide(view);
        if (action.type == ActionType::Call) ++calls;
        if (is_aggressive(action)) ++raises;
        if (action.type == ActionType::Fold) ++folds;
    }
    const double n = decisions;
    return {calls / n, raises / n, folds / n};
}

// --- Anti-cheat --------------------------------------------------------------

// What a bot on seat 1 does on the flop, when the turn and river are still undealt.
Action flop_decision(BotKind kind, const std::array<Card, 2>& opponent_cards, const char* turn, const char* river) {
    const std::array<Card, 2> mine = hole("Qh", "Jh");
    const std::array<Card, 5> board = board_of("Th", "9c", "2h", turn, river);

    auto bot = make_bot(kind, 77, 500);
    bot->begin_hand(1);
    GameEngine engine = heads_up_to_flop(opponent_cards, mine, board);

    return bot->decide(engine.player_view(1));
}

class AntiCheat : public testing::TestWithParam<BotKind> {};

TEST_P(AntiCheat, DecisionsIgnoreOpponentCardsAndUndealtBoard) {
    EXPECT_TRUE(same(flop_decision(GetParam(), hole("As", "Ad"), "3d", "4d"),
                     flop_decision(GetParam(), hole("7c", "2s"), "Kd", "Ks")));
}

// Every street, with bots that keep drawing from their RNG between decisions.
TEST_P(AntiCheat, WholeHandDecisionsIgnoreOpponentCards) {
    const std::array<Card, 5> board = board_of("Th", "9c", "2h", "3d", "4d");
    const std::array<Card, 2> mine = hole("Qh", "Jh");

    std::vector<Action> first;
    for (int run = 0; run < 2; ++run) {
        auto bot = make_bot(GetParam(), 99, 500);
        bot->begin_hand(1);
        GameEngine engine = heads_up_to_flop(run == 0 ? hole("As", "Ad") : hole("7c", "2s"), mine, board);

        std::vector<Action> seen;
        for (int street = 0; street < 3; ++street) {
            for (int i = 0; i < 20; ++i) seen.push_back(bot->decide(engine.player_view(1)));
            check_street(engine);
        }
        for (int i = 0; i < 20; ++i) seen.push_back(bot->decide(engine.player_view(1)));

        if (run == 0) {
            first = seen;
        } else {
            ASSERT_EQ(first.size(), seen.size());
            for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_TRUE(same(first[i], seen[i])) << "decision " << i;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllBots, AntiCheat, testing::ValuesIn(kAllBotKinds));
INSTANTIATE_TEST_SUITE_P(PoolBots, AntiCheat, testing::ValuesIn(kExtraBotKinds));

// --- Nit ---------------------------------------------------------------------

TEST(Nit, FoldsTrashAndOpensPremium) {
    NitBot nit(1);
    GameEngine trash = six_max(3, hole("7c", "2d"));
    EXPECT_EQ(nit.decide(trash.player_view(3)).type, ActionType::Fold);
    GameEngine premium = six_max(3, hole("Ac", "Ad"));
    const Action open = nit.decide(premium.player_view(3));
    EXPECT_EQ(open.type, ActionType::Raise);
    EXPECT_EQ(open.amount, 50);  // 2.5 big blinds
}

double empirical_entry_rate(IBotStrategy& bot, int hands) {
    GameConfig config;
    config.seed = 2024;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    GameEngine engine(config);
    int entered = 0;
    for (int i = 0; i < hands; ++i) {
        engine.new_game();
        engine.start_hand();
        const Action action = bot.decide(engine.player_view(3));  // UTG, first in
        if (action.type != ActionType::Fold) ++entered;
    }
    return static_cast<double>(entered) / hands;
}

TEST(Nit, OpenFrequencyMatchesTopTwelvePercent) {
    NitBot nit(2);
    const int hands = 20000;
    const double expected = HandRange::top_percent(kNitPlayPercent).percent() / 100.0;
    EXPECT_NEAR(expected, 0.12, 0.01);
    EXPECT_NEAR(empirical_entry_rate(nit, hands), expected, tolerance(expected, hands));
}

TEST(Loose, EntryFrequencyMatchesTopFiftyFivePercent) {
    LooseBot loose(3);
    const int hands = 20000;
    const double expected = HandRange::top_percent(kLoosePlayPercent).percent() / 100.0;
    EXPECT_NEAR(expected, 0.55, 0.01);
    EXPECT_NEAR(empirical_entry_rate(loose, hands), expected, tolerance(expected, hands));
}

// High card with no draw, bet into so the price sits inside or outside the margin.
struct MarginSpot {
    std::array<Card, 2> me = hole("Qc", "4d");
    std::array<Card, 5> board = board_of("Ts", "8h", "3d", "2c", "6s");
};

// Pot 200 and a bet of b gives pot odds b / (200 + 2b).
int bet_for_odds(double odds) { return static_cast<int>(std::lround(200.0 * odds / (1.0 - 2.0 * odds))); }

constexpr int kMarginRollouts = 20000;  // Noise of about 0.0035, against probes 0.03 from the margin

TEST(Nit, CallsOnlyWithEquityClearOfTheMargin) {
    const MarginSpot spot;
    const std::array<Card, 3> flop{spot.board[0], spot.board[1], spot.board[2]};
    const double equity = equity_of(spot.me, flop, 200000);
    ASSERT_GT(equity, 0.28);  // The spot only works for a hand around here
    ASSERT_LT(equity, 0.42);

    // The specified 0.15, written out so that the constant cannot drift with the code.
    const int inside_margin = bet_for_odds(equity - 0.15 + 0.03);
    const int clear_of_margin = bet_for_odds(equity - 0.15 - 0.03);
    ASSERT_LT(inside_margin, 150) << "must stay under the large-bet rule, which is a different test";

    NitBot nit(4, kMarginRollouts);
    GameEngine a = heads_up_to_flop(spot.me, hole("Ks", "Kd"), spot.board);
    ASSERT_TRUE(a.apply_action({ActionType::Bet, inside_margin}));
    GameEngine b = heads_up_to_flop(spot.me, hole("Ks", "Kd"), spot.board);
    ASSERT_TRUE(b.apply_action({ActionType::Bet, clear_of_margin}));
    EXPECT_NEAR(pot_math(a.player_view(0)).pot_odds, equity - 0.12, 0.003);
    EXPECT_NEAR(pot_math(b.player_view(0)).pot_odds, equity - 0.18, 0.003);

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(nit.decide(a.player_view(0)).type, ActionType::Fold);
        EXPECT_EQ(nit.decide(b.player_view(0)).type, ActionType::Call);
    }
}

TEST(Nit, FoldsNonNuttedToLargeRaisesButNotASet) {
    NitBot nit(5, kScenarioRollouts);
    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");

    GameEngine top_pair = heads_up_to_flop(hole("Ks", "Qh"), hole("9c", "9d"), board);
    ASSERT_TRUE(top_pair.apply_action({ActionType::Bet, 250}));
    EXPECT_EQ(nit.decide(top_pair.player_view(0)).type, ActionType::Fold);

    GameEngine set = heads_up_to_flop(hole("8c", "8d"), hole("9c", "9d"), board);
    ASSERT_TRUE(set.apply_action({ActionType::Bet, 250}));
    EXPECT_NE(nit.decide(set.player_view(0)).type, ActionType::Fold);
}

TEST(Nit, FoldsPreflopToAShoveWithoutAPremium) {
    NitBot nit(6);
    GameEngine engine = six_max(2, hole("Ac", "Jc"));  // Top 12%, not premium
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    fold_until(engine, 2);
    EXPECT_EQ(nit.decide(engine.player_view(2)).type, ActionType::Fold);
}

// --- Loose vs Nit ------------------------------------------------------------

TEST(Loose, CallsWeakPairWhereNitFolds) {
    const std::array<Card, 5> board = board_of("Kd", "Ts", "3c", "2h", "Jd");
    GameEngine engine = heads_up_to_flop(hole("3h", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 200}));
    const PlayerView view = engine.player_view(0);

    NitBot nit(7, kScenarioRollouts);
    LooseBot loose(8, kScenarioRollouts);
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(nit.decide(view).type, ActionType::Fold);
        EXPECT_NE(loose.decide(view).type, ActionType::Fold);
    }
}

TEST(Loose, CallsWeakDrawWhereNitFolds) {
    // A gutshot and nothing else, facing a pot-sized bet.
    const std::array<Card, 5> board = board_of("9d", "8s", "6c", "2h", "Kd");
    GameEngine engine = heads_up_to_flop(hole("5h", "2d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 200}));
    const PlayerView view = engine.player_view(0);

    NitBot nit(9, kScenarioRollouts);
    LooseBot loose(10, kScenarioRollouts);
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(nit.decide(view).type, ActionType::Fold);
        EXPECT_NE(loose.decide(view).type, ActionType::Fold);
    }
}

TEST(Loose, FoldsAirBelowItsEquityBar) {
    // Pot odds of 40% less the 12% margin needs 28%, above what this air holds.
    const std::array<Card, 5> board = board_of("Ks", "Qh", "Td", "2c", "8s");
    const std::array<Card, 2> air = hole("3c", "4d");
    const std::array<Card, 3> flop{board[0], board[1], board[2]};
    ASSERT_LT(equity_of(air, flop), 0.28);

    GameEngine engine = heads_up_to_flop(air, hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 400}));
    const PlayerView view = engine.player_view(0);

    LooseBot loose(11, kScenarioRollouts);
    for (int i = 0; i < 200; ++i) {
        EXPECT_NE(loose.decide(view).type, ActionType::Call);
    }
}

TEST(Loose, CallsAHighCardHandInsideTheMarginAndNotBeyondIt) {
    // No pair and no draw, so only the equity rule can produce a call.
    const std::array<Card, 5> board = board_of("Ks", "Qh", "Td", "2c", "8s");
    const std::array<Card, 2> air = hole("3c", "4d");
    const std::array<Card, 3> flop{board[0], board[1], board[2]};
    const HandRead read = read_hand(air, flop);
    ASSERT_FALSE(read.holds_pair);
    ASSERT_FALSE(read.has_draw());
    const double equity = equity_of(air, flop, 200000);
    ASSERT_LT(equity, 0.30) << "the far probe needs pot odds that exist heads-up";

    // The specified 0.12, written out so that the constant cannot drift with the code.
    GameEngine inside = heads_up_to_flop(air, hole("Ac", "Ad"), board);
    ASSERT_TRUE(inside.apply_action({ActionType::Bet, bet_for_odds(equity + 0.12 - 0.06)}));
    GameEngine beyond = heads_up_to_flop(air, hole("Ac", "Ad"), board);
    ASSERT_TRUE(beyond.apply_action({ActionType::Bet, bet_for_odds(equity + 0.12 + 0.06)}));

    LooseBot loose(12, kMarginRollouts);
    for (int i = 0; i < 30; ++i) {
        EXPECT_NE(loose.decide(inside.player_view(0)).type, ActionType::Fold);
        EXPECT_NE(loose.decide(beyond.player_view(0)).type, ActionType::Call);
    }
}

TEST(Loose, AnyPairCallsEvenForItsWholeStack) {
    // CLAUDE.md gives "holding any pair/draw" no exception for the size of the call.
    const std::array<Card, 5> board = board_of("Ks", "Qh", "Td", "2c", "8s");
    const std::array<Card, 2> weak = hole("2d", "3h");
    GameEngine turn = heads_up_to_flop(weak, hole("Ac", "Ad"), board);
    check_street(turn);  // The 2c pairs on the turn
    ASSERT_TRUE(turn.apply_action({ActionType::AllIn, 0}));
    const PlayerView view = turn.player_view(0);
    ASSERT_FALSE(view.legal.allows(ActionType::Raise));

    LooseBot loose(12, kFastRollouts);
    const Answers rates = answer_rates(loose, view, 300);
    EXPECT_DOUBLE_EQ(rates.calls, 1.0);
}

// --- Bluff frequencies -------------------------------------------------------

double bluff_rate(IBotStrategy& bot, const PlayerView& view, int decisions) {
    int bets = 0;
    for (int i = 0; i < decisions; ++i) {
        if (is_aggressive(bot.decide(view))) ++bets;
    }
    return static_cast<double>(bets) / decisions;
}

TEST(BluffFrequency, NitAndLooseMatchTheirProfiles) {
    const std::array<Card, 5> board = board_of("As", "Kh", "9d", "2c", "Js");
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("7c", "3d"), board);
    const PlayerView view = engine.player_view(1);  // Checked to, with air no value bar reaches
    constexpr int kDecisions = 4000;

    NitBot nit(13, kFastRollouts);
    EXPECT_NEAR(bluff_rate(nit, view, kDecisions), kNitBluffFreq, tolerance(kNitBluffFreq, kDecisions));
    LooseBot loose(14, kFastRollouts);
    EXPECT_NEAR(bluff_rate(loose, view, kDecisions), kLooseBluffFreq, tolerance(kLooseBluffFreq, kDecisions));
}

TEST(BluffFrequency, GtoBluffsDrawsMoreThanAirAndMatchesItsShare) {
    constexpr int kDecisions = 4000;
    const std::array<Card, 5> board = board_of("Ks", "7s", "2d", "9c", "4h");

    // Below the value bar with a flush draw, against the same hand with air.
    GameEngine draw_engine = heads_up_to_flop(hole("Tc", "Td"), hole("5s", "3s"), board);
    GameEngine air_engine = heads_up_to_flop(hole("Tc", "Td"), hole("5c", "3d"), board);

    GtoBot gto(15, kFastRollouts);
    const double draw_rate = bluff_rate(gto, draw_engine.player_view(1), kDecisions);
    const double air_rate = bluff_rate(gto, air_engine.player_view(1), kDecisions);

    // Two-tone with cards to come: a flush draw is live, as this hand shows.
    ASSERT_TRUE(classify_board(draw_engine.state().board).wet);
    const double s = kWetBetFraction;
    const double share = s / (1.0 + 2.0 * s);
    EXPECT_NEAR(draw_rate, 2.0 * share, tolerance(2.0 * share, kDecisions));
    EXPECT_NEAR(air_rate, 0.25 * share, tolerance(0.25 * share, kDecisions));
}

TEST(BluffFrequency, GtoNeverBluffsThreeOpponents) {
    GameEngine engine = six_max(2, hole("Kh", "3d"));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));  // UTG, HJ, CO
    fold_until(engine, 2);
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_EQ(engine.state().street, Street::Flop);
    fold_until(engine, 2);
    const PlayerView view = engine.player_view(2);
    ASSERT_EQ(view.opponents_in_hand(), 3);
    ASSERT_FALSE(read_hand(*view.own_hole, view.board).has_draw());

    GtoBot gto(40, kFastRollouts);
    EXPECT_DOUBLE_EQ(bluff_rate(gto, view, 500), 0.0);
}

TEST(BluffFrequency, FacingABetLooseRaisesAtItsShareOfBluffFreq) {
    const std::array<Card, 5> board = board_of("Ks", "Qh", "Td", "2c", "8s");
    GameEngine engine = heads_up_to_flop(hole("3c", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 400}));
    constexpr int kDecisions = 4000;
    const double expected = kLooseBluffFreq * 0.25;
    LooseBot loose(41, kFastRollouts);
    EXPECT_NEAR(bluff_rate(loose, engine.player_view(0), kDecisions), expected, tolerance(expected, kDecisions));
}

TEST(BluffFrequency, FacingABetGtoRaisesStrongDrawsAtItsRate) {
    const std::array<Card, 5> board = board_of("Ks", "7s", "2d", "9c", "4h");
    GameEngine engine = heads_up_to_flop(hole("5s", "3s"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));
    constexpr int kDecisions = 4000;
    GtoBot gto(42, kFastRollouts);
    EXPECT_NEAR(bluff_rate(gto, engine.player_view(0), kDecisions), 0.15, tolerance(0.15, kDecisions));
}

TEST(BluffFrequency, ABluffRaiseThatCannotBeMadeIsNotACall) {
    const std::array<Card, 5> kqt = board_of("Ks", "Qh", "Td", "2c", "8s");
    GameEngine loose_spot = heads_up_to_flop(hole("4c", "3d"), hole("Ac", "Ad"), kqt);
    ASSERT_TRUE(loose_spot.apply_action({ActionType::AllIn, 0}));
    LooseBot loose(43, kFastRollouts);
    EXPECT_DOUBLE_EQ(answer_rates(loose, loose_spot.player_view(0), 1000).folds, 1.0);

    const std::array<Card, 5> two_tone = board_of("Ks", "7s", "2d", "9c", "4h");
    GameEngine gto_spot = heads_up_to_flop(hole("5s", "3s"), hole("Ac", "Ad"), two_tone);
    ASSERT_TRUE(gto_spot.apply_action({ActionType::AllIn, 0}));
    GtoBot gto(44, kDefaultRollouts);  // Far enough below the bluff-catching band that noise cannot reach it
    EXPECT_DOUBLE_EQ(answer_rates(gto, gto_spot.player_view(0), 1000).folds, 1.0);
}

// --- Later knobs: sizing, position, slowplay, continuation bets -------------

TEST(Profiles, ClassicProfilesKeepDefaultKnobs) {
    for (const PersonalityProfile& p : {nit_profile(), loose_profile(), gto_profile()}) {
        SCOPED_TRACE(std::string(p.name));
        EXPECT_EQ(p.positional_width, 0.0);
        EXPECT_EQ(p.slowplay_freq, 0.0);
        EXPECT_EQ(p.cbet_freq, 0.0);
        EXPECT_EQ(p.preflop_sizing.open_bb, 2.5);
        EXPECT_EQ(p.preflop_sizing.vs_raise_mult, 3.0);
        EXPECT_EQ(p.preflop_sizing.vs_reraise_mult, 2.3);
        EXPECT_EQ(p.bet_sizes.dry, kDryBetFraction);
        EXPECT_EQ(p.bet_sizes.wet, kWetBetFraction);
        EXPECT_EQ(p.bet_sizes.polarized, kPolarizedBetFraction);
    }
}

TEST(Knobs, PositionalWidthScalesTheEntryCutoffs) {
    PersonalityProfile p = loose_profile();
    EXPECT_EQ(cutoffs_at(p, Position::UTG).play, p.cutoffs.play);  // Off: the same everywhere

    p.positional_width = 0.6;
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::UTG).play, kLoosePlayPercent * 0.7);
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::CO).play, kLoosePlayPercent);
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::BTN).play, kLoosePlayPercent * 1.36);
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::BTN).open_raise, 20.0 * 1.36);
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::BTN).four_bet, p.cutoffs.four_bet);  // Not scaled
    EXPECT_DOUBLE_EQ(cutoffs_at(p, std::nullopt).play, kLoosePlayPercent);

    p.positional_width = 3.0;  // Capped at 100, and never below a fifth
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::BTN).play, 100.0);
    EXPECT_DOUBLE_EQ(cutoffs_at(p, Position::UTG).play, kLoosePlayPercent * 0.2);
}

// First-in entry rate from UTG (seat 3) or, with the three before it folding, the button (seat 0).
double entry_rate_at(IBotStrategy& bot, int seat, int hands) {
    GameConfig config;
    config.seed = 2025;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    GameEngine engine(config);
    int entered = 0;
    for (int i = 0; i < hands; ++i) {
        engine.new_game();
        engine.start_hand();
        while (engine.state().to_act != seat) {
            if (!engine.apply_action({ActionType::Fold, 0})) return -1.0;
        }
        if (bot.decide(engine.player_view(seat)).type != ActionType::Fold) ++entered;
    }
    return static_cast<double>(entered) / hands;
}

TEST(Knobs, PositionalWidthPlaysWiderOnTheButton) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.positional_width = 0.6;
    ProfileBot bot(p, 60);
    constexpr int kHands = 20000;
    const double utg = HandRange::top_percent(kLoosePlayPercent * 0.7).percent() / 100.0;
    const double btn = HandRange::top_percent(kLoosePlayPercent * 1.36).percent() / 100.0;
    EXPECT_NEAR(entry_rate_at(bot, 3, kHands), utg, tolerance(utg, kHands));
    EXPECT_NEAR(entry_rate_at(bot, 0, kHands), btn, tolerance(btn, kHands));
}

TEST(Knobs, PreflopSizing) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.preflop_sizing = {.open_bb = 3.5, .vs_raise_mult = 4.0, .vs_reraise_mult = 2.6};
    ProfileBot bot(p, 61);

    GameEngine open = six_max(3, hole("Ac", "Ad"));
    EXPECT_TRUE(same(bot.decide(open.player_view(3)), {ActionType::Raise, 70}));

    GameEngine three_bet = six_max(4, hole("Ac", "Ad"));
    ASSERT_TRUE(three_bet.apply_action({ActionType::Raise, 50}));  // UTG opens
    EXPECT_TRUE(same(bot.decide(three_bet.player_view(4)), {ActionType::Raise, 200}));

    GameEngine four_bet = six_max(3, hole("Ac", "Ad"));
    ASSERT_TRUE(four_bet.apply_action({ActionType::Raise, 50}));
    ASSERT_TRUE(four_bet.apply_action({ActionType::Raise, 150}));  // HJ 3-bets
    fold_until(four_bet, 3);
    EXPECT_TRUE(same(bot.decide(four_bet.player_view(3)), {ActionType::Raise, 390}));  // 2.6 x 150
}

TEST(Knobs, BetSizesByTexture) {
    // Pot 200, seat 1 first to act: dry 75% = 150, wet 125% = 250, river polarized 200% = 400.
    PersonalityProfile p = gto_profile(kScenarioRollouts);
    p.bet_sizes = {.dry = 0.75, .wet = 1.25, .polarized = 2.0};
    ProfileBot bot(p, 62);

    GameEngine dry = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    EXPECT_TRUE(same(bot.decide(dry.player_view(1)), {ActionType::Bet, 150}));

    GameEngine wet = heads_up_to_flop(hole("Ac", "Ad"), hole("9s", "9d"), board_of("9h", "8h", "6c", "2d", "3s"));
    EXPECT_TRUE(same(bot.decide(wet.player_view(1)), {ActionType::Bet, 250}));

    GameEngine river = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    check_street(river);
    check_street(river);
    EXPECT_TRUE(same(bot.decide(river.player_view(1)), {ActionType::Bet, 400}));
}

TEST(Knobs, SlowplayChecksAVeryStrongHandAtItsRate) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.slowplay_freq = 0.4;
    ProfileBot bot(p, 63);
    constexpr int kDecisions = 4000;
    // Top set, checked to on the flop: far above the polarized bar.
    GameEngine flop = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    const double bets = bluff_rate(bot, flop.player_view(1), kDecisions);
    EXPECT_NEAR(bets, 0.6, tolerance(0.6, kDecisions));

    // Never on the river, where there is no later street to raise on.
    GameEngine river = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    check_street(river);
    check_street(river);
    EXPECT_DOUBLE_EQ(bluff_rate(bot, river.player_view(1), 500), 1.0);
}

TEST(Knobs, SlowplayFlatsInsteadOfRaising) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.slowplay_freq = 0.4;
    ProfileBot bot(p, 64);
    constexpr int kDecisions = 4000;
    GameEngine engine = heads_up_to_flop(hole("Ac", "Ad"), hole("Tc", "Td"), board_of("As", "7h", "2d", "9c", "4s"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));
    const Answers rates = answer_rates(bot, engine.player_view(0), kDecisions);
    EXPECT_NEAR(rates.calls, 0.4, tolerance(0.4, kDecisions));
    EXPECT_NEAR(rates.raises, 0.6, tolerance(0.6, kDecisions));
}

TEST(Knobs, ContinuationBetsAfterLeadingThePreviousStreet) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.bluff_freq = 0.1;
    p.cbet_freq = 0.6;
    constexpr int kDecisions = 4000;
    const std::array<Card, 5> board = board_of("Ks", "Qh", "9d", "2c", "5s");

    // Seat 0 raised preflop and is checked to on the flop with air.
    {
        ProfileBot bot(p, 65);
        GameEngine engine = heads_up_to_flop(hole("7c", "3d"), hole("Tc", "Td"), board);
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        const double expected = 0.6 + 0.4 * 0.1;
        EXPECT_NEAR(bluff_rate(bot, engine.player_view(0), kDecisions), expected, tolerance(expected, kDecisions));
    }
    // A limped pot has no aggressor: only the bluff frequency is left.
    {
        ProfileBot bot(p, 66);
        GameEngine engine = heads_up_to_flop(hole("7c", "3d"), hole("Tc", "Td"), board, 0);
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        EXPECT_NEAR(bluff_rate(bot, engine.player_view(0), kDecisions), 0.1, tolerance(0.1, kDecisions));
    }
    // A checked-through flop ends the lead: no continuation bet on the turn.
    {
        ProfileBot bot(p, 67);
        GameEngine engine = heads_up_to_flop(hole("7c", "3d"), hole("Tc", "Td"), board);
        check_street(engine);
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_EQ(engine.state().street, Street::Turn);
        EXPECT_NEAR(bluff_rate(bot, engine.player_view(0), kDecisions), 0.1, tolerance(0.1, kDecisions));
    }
}

TEST(Knobs, NoContinuationBetIntoThreeOpponents) {
    PersonalityProfile p = loose_profile(kFastRollouts);
    p.bluff_freq = 0.1;
    p.cbet_freq = 0.6;
    ProfileBot bot(p, 68);
    GameEngine engine = six_max(3, hole("Kc", "3d"));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));  // UTG
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));  // HJ, CO, BTN
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));  // SB
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));  // BB
    ASSERT_EQ(engine.state().street, Street::Flop);
    ASSERT_EQ(engine.state().to_act, 3);
    const PlayerView view = engine.player_view(3);
    ASSERT_EQ(view.opponents_in_hand(), 3);
    ASSERT_FALSE(read_hand(*view.own_hole, view.board).holds_pair);
    constexpr int kDecisions = 4000;
    EXPECT_NEAR(bluff_rate(bot, view, kDecisions), 0.1, tolerance(0.1, kDecisions));
}

// --- Pool archetypes ---------------------------------------------------------

TEST(Archetypes, StationCallsAWeakDrawWhereTagDoesNot) {
    // A gutshot and nothing else, facing a pot-sized bet.
    const std::array<Card, 5> board = board_of("9d", "8s", "6c", "2h", "Kd");
    GameEngine engine = heads_up_to_flop(hole("5h", "2d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 200}));
    const PlayerView view = engine.player_view(0);
    ProfileBot station(station_profile(kScenarioRollouts), 70);
    ProfileBot tag(tag_profile(kScenarioRollouts), 71);
    for (int i = 0; i < 40; ++i) {
        EXPECT_EQ(station.decide(view).type, ActionType::Call);
        EXPECT_NE(tag.decide(view).type, ActionType::Call);  // Folds, bar the odd bluff raise
    }
}

TEST(Archetypes, StationLimpsWhatItDoesNotRaise) {
    ProfileBot station(station_profile(kFastRollouts), 72);
    constexpr int kHands = 20000;
    const double entry = HandRange::top_percent(60.0).percent() / 100.0;
    EXPECT_NEAR(empirical_entry_rate(station, kHands), entry, tolerance(entry, kHands));
    GameEngine engine = six_max(3, cards_of("Q8o"));
    ASSERT_GT(strength_percentile(HandClass::parse("Q8o").value()), 8.0);
    ASSERT_LE(strength_percentile(HandClass::parse("Q8o").value()), 60.0);
    EXPECT_EQ(station.decide(engine.player_view(3)).type, ActionType::Call);
}

TEST(Archetypes, ManiacBluffsAndBluffRaisesOften) {
    constexpr int kDecisions = 4000;
    ProfileBot maniac(maniac_profile(kFastRollouts), 73);

    GameEngine checked_to = heads_up_to_flop(hole("Tc", "Td"), hole("7c", "3d"), board_of("As", "Kh", "9d", "2c", "Js"));
    EXPECT_NEAR(bluff_rate(maniac, checked_to.player_view(1), kDecisions), 0.55, tolerance(0.55, kDecisions));

    GameEngine facing = heads_up_to_flop(hole("3c", "4d"), hole("Ac", "Ad"), board_of("Ks", "Qh", "Td", "2c", "8s"));
    ASSERT_TRUE(facing.apply_action({ActionType::Bet, 400}));
    const double raise = 0.55 * 0.45;
    EXPECT_NEAR(bluff_rate(maniac, facing.player_view(0), kDecisions), raise, tolerance(raise, kDecisions));
}

TEST(Archetypes, RockNeverBluffsAndCallsTopPairWhereNitFolds) {
    ProfileBot rock(rock_profile(kScenarioRollouts), 74);
    GameEngine air = heads_up_to_flop(hole("Tc", "Td"), hole("7c", "3d"), board_of("As", "Kh", "9d", "2c", "Js"));
    EXPECT_DOUBLE_EQ(bluff_rate(rock, air.player_view(1), 500), 0.0);

    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");
    GameEngine top_pair = heads_up_to_flop(hole("Ks", "Qh"), hole("9c", "9d"), board);
    ASSERT_TRUE(top_pair.apply_action({ActionType::Bet, 250}));
    NitBot nit(75, kScenarioRollouts);
    EXPECT_EQ(nit.decide(top_pair.player_view(0)).type, ActionType::Fold);
    EXPECT_NE(rock.decide(top_pair.player_view(0)).type, ActionType::Fold);
}

TEST(Archetypes, LagOpensMoreThanTagAndBothWidenOnTheButton) {
    ProfileBot lag(lag_profile(kFastRollouts), 76);
    ProfileBot tag(tag_profile(kFastRollouts), 77);
    constexpr int kHands = 8000;
    const double lag_btn = entry_rate_at(lag, 0, kHands);
    const double tag_btn = entry_rate_at(tag, 0, kHands);
    EXPECT_GT(lag_btn, tag_btn + 0.08);
    EXPECT_GT(tag_btn, entry_rate_at(tag, 3, kHands) + 0.05);
}

TEST(Archetypes, TrapperSlowplaysAndBullyOverbets) {
    constexpr int kDecisions = 2000;
    GameEngine top_set = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    ProfileBot trapper(trapper_profile(kFastRollouts), 78);
    EXPECT_NEAR(bluff_rate(trapper, top_set.player_view(1), kDecisions), 0.4, tolerance(0.4, kDecisions));

    ProfileBot bully(bully_profile(kScenarioRollouts), 79);
    GameEngine river = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    check_street(river);
    check_street(river);
    EXPECT_TRUE(same(bully.decide(river.player_view(1)), {ActionType::Bet, 400}));  // Twice the pot
}

// --- Tilter ------------------------------------------------------------------

// The UTG view with a chosen hand number and stack: K6o, which TAG folds there and
// the Maniac opens.
PlayerView tilt_view(int hand_number, int stack) {
    GameEngine engine = six_max(3, cards_of("K6o"));
    PlayerView view = engine.player_view(3);
    view.hand_number = hand_number;
    view.seats[3].stack = stack;
    return view;
}

TEST(Tilter, TiltsAfterABigLossAndCoolsDown) {
    const double k6o = strength_percentile(HandClass::parse("K6o").value());
    ASSERT_GT(k6o, cutoffs_at(tag_profile(), Position::UTG).open_raise);
    ASSERT_LE(k6o, cutoffs_at(maniac_profile(), Position::UTG).open_raise);

    TiltBot bot(80, kFastRollouts);
    bot.begin_hand(1);
    EXPECT_EQ(bot.decide(tilt_view(1, 2000)).type, ActionType::Fold);
    EXPECT_EQ(bot.hand_log_label(), "Tilt:Off");

    bot.begin_hand(2);
    EXPECT_EQ(bot.decide(tilt_view(2, 1300)).type, ActionType::Raise);  // 35 big blinds down
    EXPECT_EQ(bot.hand_log_label(), "Tilt:On");
    for (int hand = 3; hand <= 9; ++hand) {
        bot.begin_hand(hand);
        EXPECT_EQ(bot.decide(tilt_view(hand, 1300)).type, ActionType::Raise) << hand;
        EXPECT_TRUE(bot.tilted()) << hand;
    }
    bot.begin_hand(10);
    EXPECT_EQ(bot.decide(tilt_view(10, 1300)).type, ActionType::Fold);
    EXPECT_EQ(bot.hand_log_label(), "Tilt:Off");
}

TEST(Tilter, ASmallerLossDoesNotTilt) {
    TiltBot bot(81, kFastRollouts);
    bot.begin_hand(1);
    (void)bot.decide(tilt_view(1, 2000));
    bot.begin_hand(2);
    EXPECT_EQ(bot.decide(tilt_view(2, 1420)).type, ActionType::Fold);  // 29 big blinds down
    EXPECT_FALSE(bot.tilted());
}

TEST(Tilter, ANewSessionResetsIt) {
    TiltBot bot(82, kFastRollouts);
    bot.begin_hand(1);
    (void)bot.decide(tilt_view(1, 2000));
    bot.begin_hand(2);
    (void)bot.decide(tilt_view(2, 1300));
    ASSERT_TRUE(bot.tilted());

    bot.begin_hand(1);  // Hand numbers restart
    EXPECT_FALSE(bot.tilted());
    EXPECT_EQ(bot.decide(tilt_view(1, 2000)).type, ActionType::Fold);
    EXPECT_FALSE(bot.tilted());
}

TEST(Tilter, TheLabelHoldsInAHandItNeverActsIn) {
    TiltBot bot(83, kFastRollouts);
    bot.begin_hand(1);
    (void)bot.decide(tilt_view(1, 2000));
    bot.begin_hand(2);
    (void)bot.decide(tilt_view(2, 1300));
    bot.begin_hand(3);  // A walk in the big blind: no decision
    EXPECT_EQ(bot.hand_log_label(), "Tilt:On");
}

// --- Random lineup -----------------------------------------------------------

TEST(RandomLineup, FiveDistinctKindsFromThePool) {
    std::mt19937_64 rng(90);
    for (int i = 0; i < 500; ++i) {
        const std::array<BotKind, 5> lineup = draw_random_lineup(rng);
        std::set<BotKind> seen(lineup.begin(), lineup.end());
        EXPECT_EQ(seen.size(), 5u);
        for (const BotKind kind : lineup) {
            EXPECT_NE(std::find(kRandomPoolKinds.begin(), kRandomPoolKinds.end(), kind), kRandomPoolKinds.end());
        }
    }
}

TEST(RandomLineup, SameSeedSameLineup) {
    std::mt19937_64 a(91);
    std::mt19937_64 b(91);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(draw_random_lineup(a), draw_random_lineup(b));
}

TEST(RandomLineup, CoversThePoolUniformly) {
    constexpr int kDraws = 13000;
    std::mt19937_64 rng(92);
    std::map<BotKind, int> anywhere;
    std::map<BotKind, int> first_place;
    for (int i = 0; i < kDraws; ++i) {
        const std::array<BotKind, 5> lineup = draw_random_lineup(rng);
        for (const BotKind kind : lineup) ++anywhere[kind];
        ++first_place[lineup[0]];
    }
    const double in_lineup = 5.0 / 13.0;
    const double in_place = 1.0 / 13.0;
    for (const BotKind kind : kRandomPoolKinds) {
        SCOPED_TRACE(std::string(bot_kind_name(kind)));
        EXPECT_NEAR(anywhere[kind] / static_cast<double>(kDraws), in_lineup, tolerance(in_lineup, kDraws));
        EXPECT_NEAR(first_place[kind] / static_cast<double>(kDraws), in_place, tolerance(in_place, kDraws));
    }
}

// --- GTO ---------------------------------------------------------------------

TEST(Gto, SameHandOpensButtonAndFoldsUtg) {
    GtoBot gto(16);
    const std::array<Card, 2> a9o = hole("Ac", "9d");
    ASSERT_FALSE(should_open(HandClass::from_cards(a9o[0], a9o[1]), Position::UTG));
    ASSERT_TRUE(should_open(HandClass::from_cards(a9o[0], a9o[1]), Position::BTN));

    GameEngine utg = six_max(3, a9o);
    EXPECT_EQ(gto.decide(utg.player_view(3)).type, ActionType::Fold);

    GameEngine btn = six_max(0, a9o);
    fold_until(btn, 0);
    EXPECT_EQ(gto.decide(btn.player_view(0)).type, ActionType::Raise);
}

TEST(Gto, EveryPositionFollowsTheOpeningChart) {
    GtoBot gto(17);
    const std::array<std::pair<int, Position>, 5> seats{
        {{3, Position::UTG}, {4, Position::HJ}, {5, Position::CO}, {0, Position::BTN}, {1, Position::SB}}};
    const std::array<std::array<const char*, 2>, 6> hands{
        {{"Ac", "Ad"}, {"Kc", "Td"}, {"8h", "7h"}, {"Qc", "9d"}, {"5h", "4h"}, {"7c", "2d"}}};
    for (const auto& [seat, position] : seats) {
        for (const auto& h : hands) {
            const std::array<Card, 2> cards = hole(h[0], h[1]);
            GameEngine engine = six_max(seat, cards);
            fold_until(engine, seat);
            const PlayerView view = engine.player_view(seat);
            ASSERT_EQ(view.position, position);
            const bool opens = should_open(HandClass::from_cards(cards[0], cards[1]), position);
            EXPECT_EQ(gto.decide(view).type, opens ? ActionType::Raise : ActionType::Fold)
                << h[0] << h[1] << " from " << to_string(position);
        }
    }
}

TEST(Gto, FollowsTheChartsFacingAnOpen) {
    GtoBot gto(18);
    GameEngine three_bet = six_max(2, hole("Ac", "Ad"));
    fold_until(three_bet, 3);
    ASSERT_TRUE(three_bet.apply_action({ActionType::Raise, 60}));
    fold_until(three_bet, 2);
    const Action reraise = gto.decide(three_bet.player_view(2));
    EXPECT_EQ(reraise.type, ActionType::Raise);
    EXPECT_EQ(reraise.amount, 180);

    GameEngine fold = six_max(2, hole("7c", "2d"));
    fold_until(fold, 3);
    ASSERT_TRUE(fold.apply_action({ActionType::Raise, 60}));
    fold_until(fold, 2);
    EXPECT_EQ(gto.decide(fold.player_view(2)).type, ActionType::Fold);
}

TEST(Gto, TakesItsFreeLookInTheBigBlind) {
    GtoBot gto(19);
    GameEngine engine = six_max(2, hole("7c", "2d"));
    fold_until(engine, 1);
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    EXPECT_EQ(gto.decide(engine.player_view(2)).type, ActionType::Check);
}

TEST(Gto, SizesBetsByTexture) {
    // Pot 200, seat 1 first to act: 33% = 66, 75% = 150, 125% = 250.
    GtoBot gto(20, kScenarioRollouts);

    GameEngine dry = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    ASSERT_TRUE(classify_board(dry.state().board).dry());
    const Action dry_bet = gto.decide(dry.player_view(1));
    EXPECT_EQ(dry_bet.type, ActionType::Bet);
    EXPECT_EQ(dry_bet.amount, 66);

    GameEngine wet = heads_up_to_flop(hole("Ac", "Ad"), hole("9s", "9d"), board_of("9h", "8h", "6c", "2d", "3s"));
    ASSERT_TRUE(classify_board(wet.state().board).wet);
    const Action wet_bet = gto.decide(wet.player_view(1));
    EXPECT_EQ(wet_bet.type, ActionType::Bet);
    EXPECT_EQ(wet_bet.amount, 150);

    // The river nuts bet polarized.
    GameEngine river = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    check_street(river);
    check_street(river);
    const Action river_bet = gto.decide(river.player_view(1));
    EXPECT_EQ(river_bet.type, ActionType::Bet);
    EXPECT_EQ(river_bet.amount, 250);
}

TEST(Gto, CallsAndFoldsByPrice) {
    GtoBot gto(21, kScenarioRollouts);
    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");

    GameEngine top_pair = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(top_pair.apply_action({ActionType::Bet, 100}));
    EXPECT_NE(gto.decide(top_pair.player_view(0)).type, ActionType::Fold);

    GameEngine air = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(air.apply_action({ActionType::Bet, 400}));
    EXPECT_EQ(gto.decide(air.player_view(0)).type, ActionType::Fold);
}

TEST(Gto, DefendsMarginalHandsAtMdf) {
    // Bottom pair, with the bet sized to land inside the bluff-catching band: the MDF
    // mix decides. The spot is 3 points from each band edge and the rollouts' noise
    // is 0.7, so the binomial mix is the only randomness to allow for.
    const std::array<Card, 5> board = board_of("Ks", "Qh", "2d", "9c", "8s");
    const std::array<Card, 2> pair = hole("4c", "2c");
    const std::array<Card, 3> flop{board[0], board[1], board[2]};
    const double equity = equity_of(pair, flop, 200000);

    int bet = 0;
    for (int b = 20; b <= 1800 && bet == 0; b += 10) {
        const double odds = static_cast<double>(b) / (200.0 + 2.0 * b);
        const double adjusted = equity - 0.10 * std::min(b / 200.0, 1.5);
        if (adjusted < odds - 0.03 && adjusted > odds - 0.05) bet = b;
    }
    ASSERT_GT(bet, 0) << "No bet puts equity " << equity << " in the band";

    GameEngine engine = heads_up_to_flop(pair, hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, bet}));
    const PlayerView view = engine.player_view(0);
    const PotMath math = pot_math(view);

    constexpr int kDecisions = 1500;
    GtoBot gto(22, kDefaultRollouts);
    const Answers rates = answer_rates(gto, view, kDecisions);
    EXPECT_NEAR(rates.calls, math.mdf, tolerance(math.mdf, kDecisions));
}

// --- Preflop stack depth and price -------------------------------------------

constexpr std::array<int, kMaxSeats> kCoveringBb{2000, 2000, 8000, 2000, 2000, 2000};
constexpr std::array<int, kMaxSeats> kEvenStacks{2000, 2000, 2000, 2000, 2000, 2000};

// The big blind's answer to a 100bb UTG shove with everyone else out.
ActionType bb_versus_shove(IBotStrategy& bot, const std::array<int, kMaxSeats>& stacks, const char* hand) {
    GameEngine engine = six_max_stacks(stacks, 2, cards_of(hand));
    fold_until(engine, 3);
    EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    fold_until(engine, 2);
    return bot.decide(engine.player_view(2)).type;
}

TEST(PreflopCommit, ACoveringStackFacesTheSameShoveAsAnEvenOne) {
    for (const auto& stacks : {kCoveringBb, kEvenStacks}) {
        GtoBot gto(50);
        for (const char* hand : {"54s", "22", "A5s", "AQo", "JTs", "KQo"}) {
            EXPECT_EQ(bb_versus_shove(gto, stacks, hand), ActionType::Fold) << "GTO " << hand << " " << stacks[2];
        }
        NitBot nit(51);
        for (const char* hand : {"AQo", "JTs", "KQo"}) {
            EXPECT_EQ(bb_versus_shove(nit, stacks, hand), ActionType::Fold) << "Nit " << hand << " " << stacks[2];
        }
        LooseBot loose(52);
        for (const char* hand : {"K9o", "K6o", "A5s"}) {
            EXPECT_EQ(bb_versus_shove(loose, stacks, hand), ActionType::Fold) << "Loose " << hand << " " << stacks[2];
        }
        EXPECT_NE(bb_versus_shove(gto, stacks, "AA"), ActionType::Fold);
        EXPECT_NE(bb_versus_shove(nit, stacks, "AA"), ActionType::Fold);
        EXPECT_NE(bb_versus_shove(loose, stacks, "AA"), ActionType::Fold);
    }
}

TEST(PreflopCommit, ShortShoveCommitsEvenIfTheCallIsSmallForACoveringStack) {
    // 40bb from UTG. The call is a small share of 8000 but all of what can be lost.
    GtoBot gto(58);
    GameEngine engine = six_max_stacks({2000, 2000, 8000, 800, 2000, 2000}, 2, cards_of("JTs"));
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    fold_until(engine, 2);
    EXPECT_EQ(gto.decide(engine.player_view(2)).type, ActionType::Fold);
}

TEST(PreflopCommit, A60BigBlindCallCommitsHoweverDeepEverybodyIs) {
    GtoBot gto(59);
    GameEngine engine = six_max_stacks({8000, 8000, 8000, 8000, 8000, 8000}, 2, cards_of("JTs"));
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 1200}));
    fold_until(engine, 2);
    ASSERT_LT(engine.player_view(2).legal.call_amount * 3, 8000);
    EXPECT_EQ(gto.decide(engine.player_view(2)).type, ActionType::Fold);
}

TEST(PreflopCommit, ANormalOpenIsNotCommittingForADeepStack) {
    GtoBot gto(53);
    GameEngine engine = six_max_stacks({8000, 8000, 8000, 8000, 8000, 8000}, 2, cards_of("JTs"));
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    fold_until(engine, 2);
    EXPECT_EQ(gto.decide(engine.player_view(2)).type, ActionType::Call);
}

TEST(PreflopChart, TheCheapPriceShrinksWithMoreOpponents) {
    // 72o in the big blind after an open and three callers: 13% pot odds, but four
    // hands to beat.
    GameEngine engine = six_max_stacks(kEvenStacks, 2, cards_of("72o"));
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    fold_until(engine, 2);
    const PlayerView view = engine.player_view(2);
    ASSERT_LT(pot_math(view).pot_odds, 0.20);
    GtoBot gto(57);
    EXPECT_EQ(gto.decide(view).type, ActionType::Fold);
}

TEST(PreflopChart, ABluffRaiseWithNoRaiseAvailableFoldsAndValueCalls) {
    // A 10bb shove: below the commit size, so the single-raise chart answers.
    const std::array<int, kMaxSeats> stacks{2000, 2000, 2000, 200, 2000, 2000};
    const auto answer = [&](const char* hand) {
        GameEngine engine = six_max_stacks(stacks, 2, cards_of(hand));
        fold_until(engine, 3);
        EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        fold_until(engine, 2);
        const PlayerView view = engine.player_view(2);
        EXPECT_FALSE(view.legal.allows(ActionType::Raise));
        GtoBot gto(54);
        return gto.decide(view).type;
    };
    EXPECT_EQ(answer("A5s"), ActionType::Fold);  // In the chart as a raise, purely as a bluff
    EXPECT_EQ(answer("AKo"), ActionType::Call);  // In the chart as a raise, for value
    EXPECT_EQ(answer("AQs"), ActionType::Call);
}

TEST(PreflopChart, AnyHandDefendsAtATinyPrice) {
    // UTG all in for 30, ten more to call: 14.3% pot odds.
    const std::array<int, kMaxSeats> stacks{2000, 2000, 2000, 30, 2000, 2000};
    for (const char* hand : {"J7o", "T4o", "72o"}) {
        GameEngine engine = six_max_stacks(stacks, 2, cards_of(hand));
        fold_until(engine, 3);
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        fold_until(engine, 2);
        const PlayerView view = engine.player_view(2);
        ASSERT_NEAR(pot_math(view).pot_odds, 10.0 / 70.0, 1e-12);
        GtoBot gto(55);
        EXPECT_EQ(gto.decide(view).type, ActionType::Call) << hand;
    }

    GameEngine priced = six_max_stacks(kEvenStacks, 2, cards_of("T5o"));  // 22.2%: still the chart's fold
    fold_until(priced, 3);
    ASSERT_TRUE(priced.apply_action({ActionType::Raise, 40}));
    fold_until(priced, 2);
    GtoBot gto(56);
    EXPECT_EQ(gto.decide(priced.player_view(2)).type, ActionType::Fold);
}

// --- WildGTO -----------------------------------------------------------------

struct WildRates {
    double rolled = 0.0;
    double differs = 0.0;
};

WildRates wild_rates(const PlayerView& view, int decisions) {
    WildGtoBot wild(23, kFastRollouts);
    GtoBot gto(23, kFastRollouts);  // Same seed: same underlying GTO decisions
    int rolled = 0;
    int differs = 0;
    for (int i = 0; i < decisions; ++i) {
        const Action w = wild.decide(view);
        const Action g = gto.decide(view);
        if (wild.last_roll_overrode()) {
            ++rolled;
        } else {
            EXPECT_TRUE(same(w, g)) << "An override happened without its roll";
        }
        if (!same(w, g)) ++differs;
    }
    return {static_cast<double>(rolled) / decisions, static_cast<double>(differs) / decisions};
}

TEST(WildGto, DeviatesFromGtoAboutOneTimeInFive) {
    constexpr int kDecisions = 5000;
    GameEngine preflop = six_max(3, hole("7c", "2d"));  // GTO folds, Wild limps or overbets
    const WildRates a = wild_rates(preflop.player_view(3), kDecisions);
    EXPECT_NEAR(a.rolled, kWildOverrideProbability, tolerance(kWildOverrideProbability, kDecisions));
    EXPECT_NEAR(a.differs, kWildOverrideProbability, tolerance(kWildOverrideProbability, kDecisions));

    GameEngine premium = six_max(3, hole("Ac", "Ad"));  // GTO opens small, Wild overbets
    const WildRates b = wild_rates(premium.player_view(3), kDecisions);
    EXPECT_NEAR(b.differs, kWildOverrideProbability, tolerance(kWildOverrideProbability, kDecisions));

    GameEngine postflop =
        heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    const WildRates c = wild_rates(postflop.player_view(1), kDecisions);
    EXPECT_NEAR(c.differs, kWildOverrideProbability, tolerance(kWildOverrideProbability, kDecisions));
}

TEST(WildGto, OverbetIsTwicePot) {
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("Ac", "Ad"), board_of("As", "7h", "2d", "9c", "4s"));
    const PlayerView view = engine.player_view(1);
    WildGtoBot wild(24, kFastRollouts);
    int overbets = 0;
    for (int i = 0; i < 200; ++i) {
        const Action action = wild.decide(view);
        if (wild.last_roll_overrode()) {
            EXPECT_EQ(action.type, ActionType::Bet);
            EXPECT_EQ(action.amount, 400);
            ++overbets;
        }
    }
    EXPECT_GT(overbets, 0);
}

TEST(WildGto, LightCallsAndBluffRaisesWhereGtoFolds) {
    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");
    GameEngine engine = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 300}));
    const PlayerView view = engine.player_view(0);
    constexpr int kDecisions = 4000;
    constexpr double kCall = kWildOverrideProbability * 0.75;
    constexpr double kRaise = kWildOverrideProbability * 0.25;

    WildGtoBot wild(25, kFastRollouts);
    const Answers rates = answer_rates(wild, view, kDecisions);
    EXPECT_NEAR(rates.calls, kCall, tolerance(kCall, kDecisions));
    EXPECT_NEAR(rates.raises, kRaise, tolerance(kRaise, kDecisions));

    for (int i = 0; i < 200; ++i) {
        const Action action = wild.decide(view);
        if (is_aggressive(action)) { EXPECT_EQ(action.amount, raise_to_for_pot_fraction(view, kWildOverbetFraction)); }
    }
}

TEST(WildGto, HeroCallsAShoveAtTheOverrideRate) {
    // No raise can be made, so every override of the fold is a light call.
    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");
    GameEngine engine = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    constexpr int kDecisions = 3000;
    WildGtoBot wild(26, kFastRollouts);
    const Answers rates = answer_rates(wild, engine.player_view(0), kDecisions);
    EXPECT_NEAR(rates.calls, kWildOverrideProbability, tolerance(kWildOverrideProbability, kDecisions));
    EXPECT_DOUBLE_EQ(rates.raises, 0.0);
}

// --- Shifter -----------------------------------------------------------------

TEST(Shifter, PicksAllProfilesUniformlyAndHidesThem) {
    ShifterBot shifter(27, kFastRollouts);
    std::map<std::string, int> counts;
    constexpr int kHands = 8000;
    for (int hand = 1; hand <= kHands; ++hand) {
        shifter.begin_hand(hand);
        ++counts[shifter.hand_log_label()];
        EXPECT_EQ(shifter.name(), "Shifter");
    }
    ASSERT_EQ(counts.size(), 4u);
    for (const char* label : {"Shift:Nit", "Shift:Loose", "Shift:GTO", "Shift:WildGTO"}) {
        EXPECT_NEAR(static_cast<double>(counts[label]) / kHands, 0.25, tolerance(0.25, kHands)) << label;
    }
}

TEST(Shifter, ActuallyPlaysTheActiveProfile) {
    // A hand only Loose enters from UTG: it separates the first-in behaviours.
    const std::array<Card, 2> middling = hole("Kc", "6d");
    GameEngine engine = six_max(3, middling);
    const PlayerView view = engine.player_view(3);
    ASSERT_GT(strength_percentile(HandClass::from_cards(middling[0], middling[1])), kNitPlayPercent);
    ASSERT_LE(strength_percentile(HandClass::from_cards(middling[0], middling[1])), kLoosePlayPercent);

    ShifterBot shifter(28, kFastRollouts);
    int nit_hands = 0;
    int loose_hands = 0;
    for (int hand = 1; hand <= 400; ++hand) {
        shifter.begin_hand(hand);
        const Action action = shifter.decide(view);
        if (shifter.hand_log_label() == "Shift:Nit") {
            ++nit_hands;
            EXPECT_EQ(action.type, ActionType::Fold);
        } else if (shifter.hand_log_label() == "Shift:Loose") {
            ++loose_hands;
            EXPECT_NE(action.type, ActionType::Fold);
        }
    }
    EXPECT_GT(nit_hands, 0);
    EXPECT_GT(loose_hands, 0);
}

TEST(Factory, MakesEachBotWithItsPublicName) {
    const std::map<BotKind, std::string> names{{BotKind::Nit, "Nit"},
                                               {BotKind::Loose, "Loose"},
                                               {BotKind::Gto, "GTO"},
                                               {BotKind::WildGto, "WildGTO"},
                                               {BotKind::Shifter, "Shifter"}};
    for (const auto& [kind, name] : names) {
        const auto bot = make_bot(kind, 1, kFastRollouts);
        EXPECT_EQ(bot->name(), name);
        if (kind != BotKind::Shifter) { EXPECT_EQ(bot->hand_log_label(), name); }
    }
}

TEST(Factory, MakesEachPoolBotWithItsPublicName) {
    const std::map<BotKind, std::string> names{{BotKind::Tag, "TAG"},         {BotKind::Lag, "LAG"},
                                               {BotKind::Station, "Station"}, {BotKind::Maniac, "Maniac"},
                                               {BotKind::Rock, "Rock"},       {BotKind::Trapper, "Trapper"},
                                               {BotKind::Bully, "Bully"},     {BotKind::Tilter, "Tilter"}};
    ASSERT_EQ(names.size(), kExtraBotKinds.size());
    for (const auto& [kind, name] : names) {
        const auto bot = make_bot(kind, 1, kFastRollouts);
        EXPECT_EQ(bot->name(), name);
        EXPECT_EQ(bot->hand_log_label(), kind == BotKind::Tilter ? "Tilt:Off" : name);
    }
    for (const BotKind kind : kRandomPoolKinds) EXPECT_EQ(bot_kind_name(kind), make_bot(kind, 1, kFastRollouts)->name());
}

// --- Texture, draws and pot math ---------------------------------------------

std::vector<Card> cards(std::initializer_list<const char*> names) {
    std::vector<Card> out;
    for (const char* name : names) out.push_back(card(name));
    return out;
}

TEST(BoardTextureTest, Classifies) {
    const auto of = [](std::initializer_list<const char*> names) {
        const std::vector<Card> board = cards(names);
        return classify_board(board);
    };
    EXPECT_TRUE(of({"Kc", "7d", "2h"}).dry());
    EXPECT_TRUE(of({"Ac", "7d", "2h"}).dry());
    EXPECT_TRUE(of({"9c", "8d", "6h"}).wet);
    EXPECT_TRUE(of({"Jc", "Tc", "4d"}).wet);
    EXPECT_TRUE(of({"Kc", "7c", "2d"}).wet);              // Two of a suit, cards to come
    EXPECT_TRUE(of({"Ks", "Qd", "2s"}).wet);              // However far apart the suited cards are
    EXPECT_TRUE(of({"Kc", "7c", "2d", "9h"}).wet);        // Still live on the turn
    EXPECT_TRUE(of({"Kc", "7c", "2d", "9h", "4s"}).dry()); // Not on the river, with no third
    EXPECT_TRUE(of({"Kc", "7c", "2c", "9h", "4s"}).wet);  // A flush is possible
    EXPECT_TRUE(of({"As", "5d", "2c"}).wet);              // A 3-4 makes the wheel
    EXPECT_TRUE(of({"As", "7d", "2c"}).dry());
    EXPECT_TRUE(of({"Kc", "7c", "2c"}).monotone);
    EXPECT_TRUE(of({"Kc", "7c", "2c"}).wet);
    EXPECT_TRUE(of({"Kc", "Kd", "2h"}).paired);
    EXPECT_TRUE(of({"Kc", "Kd", "2h"}).dry());
    EXPECT_TRUE(of({}).dry());
    EXPECT_EQ(texture_name(of({"Kc", "7c", "2c"}), true), "monotone");
    EXPECT_EQ(texture_name(of({"Kc", "Kd", "2h"}), true), "paired");
    EXPECT_EQ(texture_name(of({"9c", "8d", "6h"}), true), "wet");
    EXPECT_EQ(texture_name(of({"Kc", "7d", "2h"}), true), "dry");
    EXPECT_EQ(texture_name(of({}), false), "no board");
}

TEST(HandReadTest, FindsDrawsAndPairs) {
    const auto read = [](const char* a, const char* b, std::initializer_list<const char*> names) {
        const std::vector<Card> board = cards(names);
        return read_hand(hole(a, b), board);
    };
    EXPECT_TRUE(read("As", "Ks", {"7s", "2s", "9d"}).flush_draw);
    EXPECT_FALSE(read("Ad", "Kc", {"7s", "2s", "9s"}).flush_draw);  // No hole card in the suit
    EXPECT_TRUE(read("9c", "8d", {"7s", "6h", "Kd"}).open_ended);
    EXPECT_TRUE(read("9c", "5d", {"8s", "6h", "Kd"}).gutshot);
    EXPECT_TRUE(read("Ac", "2d", {"3s", "4h", "Kd"}).gutshot);      // Wheel draw needs a five
    EXPECT_FALSE(read("Ac", "Kd", {"7s", "2h", "9d"}).has_draw());
    EXPECT_FALSE(read("As", "Ks", {"7s", "2s", "9d", "3h", "4h"}).has_draw());  // River: no cards to come
    EXPECT_TRUE(read("7c", "2d", {"7s", "Kh", "9d"}).holds_pair);
    EXPECT_TRUE(read("3c", "3d", {"7s", "Kh", "9d"}).holds_pair);
    EXPECT_FALSE(read("Ac", "2d", {"7s", "7h", "9d"}).holds_pair);  // Only the board is paired
    EXPECT_FALSE(read("8c", "Kd", {"5s", "6h", "7d", "8s"}).has_draw());  // The 8 repeats the board's draw
    EXPECT_TRUE(read("9c", "Kd", {"5s", "6h", "7d", "8s"}).category == HandCategory::Straight);
    EXPECT_TRUE(read("4c", "Kd", {"5s", "6h", "7d", "Qs"}).open_ended);  // A real hole card still counts
}

TEST(NuttedTest, RequiresAHandTheHoleCardsMake) {
    const auto nutted = [](const char* a, const char* b, std::initializer_list<const char*> names) {
        const std::vector<Card> board = cards(names);
        const std::array<Card, 2> h = hole(a, b);
        return is_nutted(read_hand(h, board), board);
    };
    EXPECT_TRUE(nutted("8c", "8d", {"8s", "Kh", "2d"}));   // Set
    EXPECT_FALSE(nutted("Ac", "Kd", {"Ah", "7h", "2d"}));  // Top pair
    EXPECT_FALSE(nutted("Ac", "Kd", {"7s", "7h", "7d"}));  // Trips on the board
    EXPECT_FALSE(nutted("2c", "3d", {"Ts", "Js", "Qs", "Ks", "As"}));  // Plays the board
    EXPECT_FALSE(nutted("2c", "3d", {"7s", "7h", "7d", "7c"}));        // Quads on the turn board
    EXPECT_TRUE(nutted("7c", "3d", {"7s", "7h", "7d", "Kc"}));         // Quads with a hole card
}

TEST(PotMathTest, ExactInAKnownSpot) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));
    const PotMath math = pot_math(engine.player_view(0));
    EXPECT_EQ(math.pot, 300);
    EXPECT_EQ(math.to_call, 100);
    EXPECT_DOUBLE_EQ(math.pot_odds, 100.0 / 400.0);
    EXPECT_DOUBLE_EQ(math.mdf, 200.0 / 300.0);
    EXPECT_DOUBLE_EQ(math.bet_fraction, 0.5);
}

TEST(PotMathTest, ShortCallOnlyPlaysForWhatItCanWin) {
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
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));  // 1000 into a 200 stack
    const PotMath math = pot_math(engine.player_view(1));
    EXPECT_EQ(math.to_call, 180);
    EXPECT_EQ(math.contestable, 220);  // 200 of the shove, plus the 20 blind
    EXPECT_DOUBLE_EQ(math.pot_odds, 180.0 / 400.0);
}

TEST(PotMathTest, AntesAreDeadMoneyInPotOddsAndTheMainLayer) {
    for (const AnteType type : {AnteType::PerPlayer, AnteType::BigBlind}) {
        GameConfig config;
        config.first_button = 0;
        config.hero_seat = std::nullopt;
        config.stacks = std::array<int, kMaxSeats>{1000, 200, 0, 0, 0, 0};
        config.ante = type == AnteType::PerPlayer ? 5 : 20;
        config.ante_type = type;
        GameEngine engine(config);
        ScriptedDeal deal;
        deal.holes[0] = hole("Ks", "9h");
        deal.holes[1] = hole("Ac", "Ad");
        deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
        engine.start_hand(deal);
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        const PlayerView view = engine.player_view(1);
        const PotMath math = pot_math(view);
        const std::vector<PotLayer> layers = pot_layers(view);
        ASSERT_EQ(layers.size(), 1u);
        if (type == AnteType::PerPlayer) {
            // Both post 5 and the BB has 175 left. Calling wins 195 of the shove, the
            // BB's own 20 blind and both antes (225), for 175: a 400 pot.
            EXPECT_EQ(math.to_call, 175);
            EXPECT_EQ(math.contestable, 225);
            EXPECT_DOUBLE_EQ(math.pot_odds, 175.0 / 400.0);
            EXPECT_EQ(layers[0].amount, 400);
        } else {
            // The BB posts 20 and a 20 ante, leaving 160: a 380 pot for a 160 call.
            EXPECT_EQ(math.to_call, 160);
            EXPECT_EQ(math.contestable, 220);
            EXPECT_DOUBLE_EQ(math.pot_odds, 160.0 / 380.0);
            EXPECT_EQ(layers[0].amount, 380);
        }
    }
}

TEST(PotMathTest, AnUnopenedPotHasNoMdf) {
    GameEngine engine = six_max(3, hole("7c", "2d"));
    const PotMath math = pot_math(engine.player_view(3));
    EXPECT_FALSE(math.facing_aggression);
    EXPECT_DOUBLE_EQ(math.mdf, 1.0);
    EXPECT_DOUBLE_EQ(math.bet_fraction, 0.0);
    EXPECT_EQ(math.to_call, 20);
}

TEST(PotMathTest, AnOpenIsMeasuredAgainstTheBlinds) {
    GameEngine engine = six_max(2, hole("7c", "2d"));
    fold_until(engine, 3);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    fold_until(engine, 2);
    const PotMath math = pot_math(engine.player_view(2));
    EXPECT_TRUE(math.facing_aggression);
    EXPECT_DOUBLE_EQ(math.mdf, 30.0 / 90.0);
    EXPECT_DOUBLE_EQ(math.bet_fraction, 2.0);
}

TEST(PotMathTest, ALiveRaiseIsMeasuredByWhatTheRaiserRisks) {
    // Bet 100 into 200, raise to 300: the raiser risks 300 to win 300.
    GameEngine engine = heads_up_to_flop(hole("Ac", "Ad"), hole("Ks", "Qh"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 300}));
    const PlayerView view = engine.player_view(1);
    const PotMath math = pot_math(view);
    EXPECT_EQ(math.to_call, 200);
    EXPECT_DOUBLE_EQ(math.mdf, 0.5);
    EXPECT_DOUBLE_EQ(math.bet_fraction, 1.0);

    NitBot nit(60, kFastRollouts);  // A pot-sized raise is a large one, whatever is left to match
    EXPECT_DOUBLE_EQ(answer_rates(nit, view, 20).folds, 1.0);
}

TEST(PotMathTest, TheBetIsCutToWhatTheStackCanMatch) {
    // Seat 0 has 100 behind in an 800 pot. Whatever is shoved over that, it faces 100.
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{500, 3000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Ks", "Qh");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 400}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    const PlayerView view = engine.player_view(0);
    const PotMath math = pot_math(view);
    EXPECT_EQ(math.to_call, 100);
    EXPECT_DOUBLE_EQ(math.bet_fraction, 100.0 / 800.0);
    EXPECT_DOUBLE_EQ(math.mdf, 800.0 / 900.0);

    NitBot nit(61, kScenarioRollouts);  // Top pair for its last 100: no large bet here
    EXPECT_DOUBLE_EQ(answer_rates(nit, view, 20).calls, 1.0);
}

TEST(PotMathTest, SprIsMeasuredAfterCalling) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    EXPECT_DOUBLE_EQ(pot_math(engine.player_view(1)).spr, 1900.0 / 200.0);
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));
    EXPECT_DOUBLE_EQ(pot_math(engine.player_view(0)).spr, 1750.0 / 500.0);
}

// A 40-chip big blind all in preflop, then 1000 bet on the river at seat 0. Button
// on seat 0, seat 1 small blind, seat 2 the short stack.
GameEngine side_pot_river(const std::array<Card, 2>& mine, const std::array<Card, 5>& board) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 40, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = mine;
    deal.board = board;
    engine.start_hand(fill_deal(deal, {0, 1, 2}));
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));   // Button
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));   // Small blind
    EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));  // Big blind, to 40
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    for (int street = 0; street < 2; ++street) {
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    EXPECT_EQ(engine.state().street, Street::River);
    EXPECT_TRUE(engine.apply_action({ActionType::Bet, 1000}));
    EXPECT_EQ(engine.state().to_act, 0);
    return engine;
}

TEST(SidePots, LayersSplitThePotByWhoCanWinIt) {
    GameEngine engine = side_pot_river(hole("Qc", "5d"), board_of("Ks", "7c", "2d", "9h", "4s"));
    const PlayerView view = engine.player_view(0);
    const std::vector<PotLayer> layers = pot_layers(view);
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].amount, 120);
    EXPECT_EQ(layers[0].opponents, 2);
    EXPECT_EQ(layers[0].all_in_opponents, 1);
    EXPECT_EQ(layers[1].amount, 2000);
    EXPECT_EQ(layers[1].opponents, 1);
    EXPECT_EQ(layers[1].all_in_opponents, 0);
    const PotMath math = pot_math(view);
    EXPECT_EQ(layers[0].amount + layers[1].amount, math.contestable + math.to_call);
}

TEST(SidePots, NoAllInMeansOneLayerAndNoExtraRollouts) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));
    const std::vector<PotLayer> layers = pot_layers(engine.player_view(0));
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0].amount, 400);
    EXPECT_EQ(layers[0].opponents, 1);
}

TEST(SidePots, ACallIsPricedOnTheSidePotAgainstItsOwnOpponents) {
    // Beats enough of one hand to call the 1000 side pot, not enough of two: treating
    // the whole pot as three-way would fold this.
    const std::array<Card, 5> board = board_of("Ks", "7c", "2d", "9h", "4s");
    const std::array<Card, 2> mine = hole("9c", "8d");
    GameEngine engine = side_pot_river(mine, board);
    const PlayerView view = engine.player_view(0);
    const PotMath math = pot_math(view);
    const double versus_one = equity_of(mine, board, 200000, 1);
    const double versus_two = equity_of(mine, board, 200000, 2);
    const double pressure = bet_pressure(math, kGtoBetPressureDiscount);
    const double layered = (120.0 * versus_two + 2000.0 * versus_one) / 2120.0;
    ASSERT_LT(versus_two - pressure, math.pot_odds);
    ASSERT_GT(layered - pressure, math.pot_odds + 0.02);

    GtoBot gto(62, kMarginRollouts);
    EXPECT_DOUBLE_EQ(answer_rates(gto, view, 20).calls, 1.0);
}

TEST(Legalize, RepairsEveryKindOfBadAction) {
    GameEngine facing = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(facing.apply_action({ActionType::Bet, 100}));
    const PlayerView a = facing.player_view(0);
    EXPECT_EQ(legalize(a, {ActionType::Check, 0}).type, ActionType::Fold);
    EXPECT_EQ(legalize(a, {ActionType::Bet, 500}).type, ActionType::Raise);
    EXPECT_EQ(legalize(a, {ActionType::Raise, 1}).amount, a.legal.min_raise_to);
    EXPECT_EQ(legalize(a, {ActionType::Raise, 99999}).amount, a.legal.max_raise_to);

    GameEngine free = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const PlayerView b = free.player_view(1);
    EXPECT_EQ(legalize(b, {ActionType::Fold, 0}).type, ActionType::Check);
    EXPECT_EQ(legalize(b, {ActionType::Call, 0}).type, ActionType::Check);
    EXPECT_EQ(legalize(b, {ActionType::Raise, 100}).type, ActionType::Bet);
}

TEST(Legalize, BoundsARaiseByTheDeepestOpponent) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{5000, 300, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Ks", "9h");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    EXPECT_EQ(clamp_raise_to(engine.player_view(0), 4000), 300);
}

// --- Legality fuzz -----------------------------------------------------------

std::array<int, kMaxSeats> random_stacks(std::mt19937_64& rng) {
    // Mostly playable with some very short ones: sub-blind stacks make the odd
    // cases (short blinds, min_raise_to == max_raise_to) show up in numbers.
    std::array<int, kMaxSeats> stacks{};
    for (int& stack : stacks) {
        switch (rng() % 5) {
            case 0: stack = 1 + static_cast<int>(rng() % 40); break;
            case 1: stack = 40 + static_cast<int>(rng() % 160); break;
            case 2: stack = 0; break;
            default: stack = 200 + static_cast<int>(rng() % 3800); break;
        }
    }
    if (stacks[0] == 0) stacks[0] = 500;
    if (stacks[3] == 0) stacks[3] = 37;
    return stacks;
}

// Bots playing bots, with every action required to be accepted by the engine.
void play_freezeouts(std::uint64_t seed, int games, int max_hands, int rollouts,
                     std::span<const BotKind> kinds = kAllBotKinds) {
    std::mt19937_64 rng(seed);
    long decisions = 0;
    for (int game = 0; game < games; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        config.stacks = random_stacks(rng);
        GameEngine engine(config);

        std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            const BotKind kind = kinds[(static_cast<std::size_t>(game) + static_cast<std::size_t>(seat)) % kinds.size()];
            bots[static_cast<std::size_t>(seat)] = make_bot(kind, rng(), rollouts);
        }

        for (int hand = 0; hand < max_hands && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            for (auto& bot : bots) bot->begin_hand(engine.state().hand_number);
            while (engine.state().to_act) {
                const int seat = *engine.state().to_act;
                const PlayerView view = engine.player_view(seat);
                const Action action = bots[static_cast<std::size_t>(seat)]->decide(view);
                ASSERT_TRUE(is_legal(view.legal, action))
                    << bots[static_cast<std::size_t>(seat)]->name() << " chose " << to_string(action.type) << " "
                    << action.amount;
                ASSERT_TRUE(engine.apply_action(action));
                ++decisions;
            }
        }
    }
    EXPECT_GT(decisions, games);
}

TEST(LegalityFuzz, BotsPlayFullFreezeouts) { play_freezeouts(1001, 60, 120, 40); }

TEST(LegalityFuzz, DefaultRolloutsPath) { play_freezeouts(1002, 1, 12, kDefaultRollouts); }

TEST(LegalityFuzz, PoolBotsPlayFullFreezeouts) { play_freezeouts(1004, 60, 120, 40, kRandomPoolKinds); }

// Random players steer the hand into shapes bots rarely reach on their own (minimum
// raises, short all-ins, closed betting), and every bot is asked what it would do.
void every_bot_is_legal_in_random_states(std::uint64_t seed, std::span<const BotKind> kinds) {
    std::mt19937_64 rng(seed);
    std::vector<std::unique_ptr<IBotStrategy>> bots;
    for (const BotKind kind : kinds) bots.push_back(make_bot(kind, rng(), 20));

    bool saw_no_raise = false;
    bool saw_min_equals_max = false;
    bool saw_raise = false;
    bool saw_bet = false;
    for (int game = 0; game < 400; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        config.stacks = random_stacks(rng);
        GameEngine engine(config);
        for (int hand = 0; hand < 12 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            for (auto& bot : bots) bot->begin_hand(engine.state().hand_number);
            while (engine.state().to_act) {
                const PlayerView view = engine.player_view(*engine.state().to_act);
                const LegalActions& legal = view.legal;
                saw_raise = saw_raise || legal.allows(ActionType::Raise);
                saw_bet = saw_bet || legal.allows(ActionType::Bet);
                const bool aggress = legal.allows(ActionType::Raise) || legal.allows(ActionType::Bet);
                saw_no_raise = saw_no_raise || !aggress;
                saw_min_equals_max = saw_min_equals_max || (aggress && legal.min_raise_to == legal.max_raise_to);

                for (auto& bot : bots) {
                    const Action action = bot->decide(view);
                    ASSERT_TRUE(is_legal(legal, action)) << bot->name() << " chose " << to_string(action.type);
                }

                std::vector<Action> options;
                if (legal.allows(ActionType::Check)) options.push_back({ActionType::Check, 0});
                if (legal.allows(ActionType::Call)) options.push_back({ActionType::Call, 0});
                if (legal.allows(ActionType::Fold)) options.push_back({ActionType::Fold, 0});
                if (legal.allows(ActionType::AllIn)) options.push_back({ActionType::AllIn, 0});
                const ActionType aggressive = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
                if (aggress) {
                    options.push_back({aggressive, legal.min_raise_to});
                    options.push_back({aggressive, legal.max_raise_to});
                }
                ASSERT_TRUE(engine.apply_action(options[static_cast<std::size_t>(rng() % options.size())]));
            }
        }
    }
    // Shows the fuzz really reached the awkward states rather than passing without them.
    EXPECT_TRUE(saw_no_raise);
    EXPECT_TRUE(saw_min_equals_max);
    EXPECT_TRUE(saw_raise);
    EXPECT_TRUE(saw_bet);
}

TEST(LegalityFuzz, EveryBotIsLegalInRandomStates) { every_bot_is_legal_in_random_states(1003, kAllBotKinds); }

TEST(LegalityFuzz, EveryPoolBotIsLegalInRandomStates) {
    every_bot_is_legal_in_random_states(1005, kRandomPoolKinds);
}

// --- Golden: the classic bots decide exactly as before ------------------------

// FNV-1a over 64-bit words.
struct Fnv1a {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    void add(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFU;
            h *= 0x100000001B3ULL;
        }
    }
    void add(const std::string& text) {
        for (const char ch : text) add(static_cast<std::uint64_t>(static_cast<unsigned char>(ch)));
        add(0xFFFFFFFFULL);
    }
};

std::uint64_t classic_bots_hash(long* decisions) {
    Fnv1a hash;
    std::mt19937_64 rng(4242);

    // Every classic bot answers every state of random play, as in the legality fuzz.
    std::vector<std::unique_ptr<IBotStrategy>> bots;
    for (const BotKind kind : kAllBotKinds) bots.push_back(make_bot(kind, rng(), 20));
    for (int game = 0; game < 150; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        config.stacks = random_stacks(rng);
        GameEngine engine(config);
        for (int hand = 0; hand < 12 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            for (auto& bot : bots) bot->begin_hand(engine.state().hand_number);
            for (std::size_t k = 0; k < bots.size(); ++k) hash.add(bots[k]->hand_log_label());
            while (engine.state().to_act) {
                const PlayerView view = engine.player_view(*engine.state().to_act);
                for (std::size_t k = 0; k < bots.size(); ++k) {
                    const Action action = bots[k]->decide(view);
                    hash.add(k);
                    hash.add(static_cast<std::uint64_t>(action.type));
                    hash.add(static_cast<std::uint64_t>(static_cast<std::int64_t>(action.amount)));
                }
                const LegalActions& legal = view.legal;
                std::array<Action, 6> options{};
                std::size_t count = 0;
                for (const ActionType type : {ActionType::Check, ActionType::Call, ActionType::Fold, ActionType::AllIn}) {
                    if (legal.allows(type)) options[count++] = {type, 0};
                }
                const bool aggress = legal.allows(ActionType::Raise) || legal.allows(ActionType::Bet);
                const ActionType aggressive = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
                if (aggress) {
                    options[count++] = {aggressive, legal.min_raise_to};
                    options[count++] = {aggressive, legal.max_raise_to};
                }
                ++*decisions;
                if (count == 0 || !engine.apply_action(options[static_cast<std::size_t>(rng() % count)])) return 0;
            }
        }
    }

    // And whole freezeouts of bots playing bots, which carry RNG state across hands.
    for (int game = 0; game < 12; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        GameEngine engine(config);
        std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> table;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            const BotKind kind = kAllBotKinds[(static_cast<std::size_t>(game) + static_cast<std::size_t>(seat)) %
                                              kAllBotKinds.size()];
            table[static_cast<std::size_t>(seat)] = make_bot(kind, rng(), 20);
        }
        for (int hand = 0; hand < 40 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            for (auto& bot : table) bot->begin_hand(engine.state().hand_number);
            for (auto& bot : table) hash.add(bot->hand_log_label());
            while (engine.state().to_act) {
                const int seat = *engine.state().to_act;
                const Action action = table[static_cast<std::size_t>(seat)]->decide(engine.player_view(seat));
                hash.add(static_cast<std::uint64_t>(seat));
                hash.add(static_cast<std::uint64_t>(action.type));
                hash.add(static_cast<std::uint64_t>(static_cast<std::int64_t>(action.amount)));
                ++*decisions;
                if (!engine.apply_action(action)) return 0;
            }
        }
    }
    return hash.h;
}

// Pins every decision of Nit, Loose, GTO, WildGTO and Shifter, so new personality
// knobs can be shown to leave them alone. The standard distributions the classic bots
// draw through are library-defined, so the constant is pinned for libstdc++ only;
// elsewhere the test prints the hash and skips. Re-pinned once for an engine rule fix
// (a lone small blind that already covers a short all-in big blind has no decision),
// which changes the random-stack states only from the first such hand on; the
// freezeouts below were checked unchanged on their own seed.
TEST(Golden, ClassicBotsDecideAsBefore) {
    long decisions = 0;
    const std::uint64_t hash = classic_bots_hash(&decisions);
    ASSERT_NE(hash, 0U) << "an action was refused";
    EXPECT_GT(decisions, 3000);
#if defined(__GLIBCXX__)
    EXPECT_EQ(hash, 0xedf9f03a0af54f68ULL) << std::hex << "hash 0x" << hash;
#else
    GTEST_SKIP() << std::hex << "no pinned value for this standard library; hash 0x" << hash;
#endif
}

// Run by hand: chips won per hand by each personality, as a sanity check on how
// they play together. Not asserted, since no target for it follows from the spec.
TEST(TableReport, DISABLED_ChipsPerHandByPersonality) {
    constexpr int kKinds = static_cast<int>(kAllBotKinds.size());
    std::array<double, kKinds> net{};
    std::array<long, kKinds> hands{};
    std::array<long, kKinds> folds{};
    std::array<long, kKinds> aggressive{};
    std::array<long, kKinds> decisions{};
    std::mt19937_64 rng(555);
    constexpr int kHandsPerRotation = 400;
    for (int rotation = 0; rotation < kKinds * 4; ++rotation) {
        std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots;
        std::array<int, kMaxSeats> kind_at{};
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            kind_at[static_cast<std::size_t>(seat)] = (rotation + seat) % kKinds;
            bots[static_cast<std::size_t>(seat)] =
                make_bot(kAllBotKinds[static_cast<std::size_t>(kind_at[static_cast<std::size_t>(seat)])], rng(), 300);
        }
        // A fresh 100bb table each hand, so nobody busts and every hand is comparable.
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        config.first_button = rotation % kMaxSeats;
        GameEngine engine(config);
        for (int hand = 0; hand < kHandsPerRotation; ++hand) {
            engine.new_game();
            engine.start_hand();
            for (auto& bot : bots) bot->begin_hand(hand + 1);
            while (engine.state().to_act) {
                const int seat = *engine.state().to_act;
                const std::size_t k = static_cast<std::size_t>(kind_at[static_cast<std::size_t>(seat)]);
                const Action action = bots[static_cast<std::size_t>(seat)]->decide(engine.player_view(seat));
                ++decisions[k];
                if (action.type == ActionType::Fold) ++folds[k];
                if (is_aggressive(action)) ++aggressive[k];
                ASSERT_TRUE(engine.apply_action(action));
            }
            for (int seat = 0; seat < kMaxSeats; ++seat) {
                const std::size_t k = static_cast<std::size_t>(kind_at[static_cast<std::size_t>(seat)]);
                net[k] += engine.state().seats[static_cast<std::size_t>(seat)].stack - config.starting_stack;
                ++hands[k];
            }
        }
    }
    for (std::size_t k = 0; k < net.size(); ++k) {
        const auto bot = make_bot(kAllBotKinds[k], 1, 1);
        std::printf("%-8s bb/100 %8.1f   fold %5.1f%%   aggressive %5.1f%%\n", std::string(bot->name()).c_str(),
                    net[k] / hands[k] / 20.0 * 100.0, 100.0 * folds[k] / decisions[k],
                    100.0 * aggressive[k] / decisions[k]);
    }
}

// Run by hand: how each pool personality plays at mixed tables, as a check that the
// archetypes really differ. VPIP and PFR are shares of hands; aggression is postflop
// bets and raises over calls. Not asserted: the profiles are judgement, not targets.
TEST(TableReport, DISABLED_PoolStats) {
    constexpr std::size_t kKinds = kRandomPoolKinds.size();
    std::array<double, kKinds> net{};
    std::array<long, kKinds> hands{};
    std::array<long, kKinds> vpip{};
    std::array<long, kKinds> pfr{};
    std::array<long, kKinds> post_aggr{};
    std::array<long, kKinds> post_calls{};
    std::array<long, kKinds> post_folds{};
    std::array<long, kKinds> post_decisions{};
    std::mt19937_64 rng(556);
    constexpr int kHandsPerTable = 300;
    for (int table = 0; table < 60; ++table) {
        const std::array<BotKind, 5> five = draw_random_lineup(rng);
        std::array<BotKind, kMaxSeats> kinds{};
        for (std::size_t i = 0; i < 5; ++i) kinds[i] = five[i];
        kinds[5] = kRandomPoolKinds[rng() % kKinds];  // A sixth, possibly a repeat
        std::array<std::size_t, kMaxSeats> index{};
        std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots;
        for (std::size_t seat = 0; seat < kMaxSeats; ++seat) {
            index[seat] = static_cast<std::size_t>(
                std::find(kRandomPoolKinds.begin(), kRandomPoolKinds.end(), kinds[seat]) - kRandomPoolKinds.begin());
            bots[seat] = make_bot(kinds[seat], rng(), 300);
        }
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        config.first_button = table % kMaxSeats;
        GameEngine engine(config);
        for (int hand = 0; hand < kHandsPerTable; ++hand) {
            engine.new_game();  // A fresh 100bb table each hand, so every hand is comparable
            engine.start_hand();
            for (auto& bot : bots) bot->begin_hand(hand + 1);
            std::array<bool, kMaxSeats> entered{};
            std::array<bool, kMaxSeats> raised{};
            while (engine.state().to_act) {
                const int seat = *engine.state().to_act;
                const std::size_t k = index[static_cast<std::size_t>(seat)];
                const Street street = engine.state().street;
                const Action action = bots[static_cast<std::size_t>(seat)]->decide(engine.player_view(seat));
                if (street == Street::Preflop) {
                    if (action.type == ActionType::Call || is_aggressive(action)) entered[static_cast<std::size_t>(seat)] = true;
                    if (is_aggressive(action)) raised[static_cast<std::size_t>(seat)] = true;
                } else {
                    ++post_decisions[k];
                    if (is_aggressive(action)) ++post_aggr[k];
                    if (action.type == ActionType::Call) ++post_calls[k];
                    if (action.type == ActionType::Fold) ++post_folds[k];
                }
                ASSERT_TRUE(engine.apply_action(action));
            }
            for (std::size_t seat = 0; seat < kMaxSeats; ++seat) {
                const std::size_t k = index[seat];
                net[k] += engine.state().seats[seat].stack - config.starting_stack;
                ++hands[k];
                if (entered[seat]) ++vpip[k];
                if (raised[seat]) ++pfr[k];
            }
        }
    }
    std::printf("%-8s %7s %6s %6s %6s %7s %8s\n", "style", "hands", "VPIP", "PFR", "AF", "fold%", "bb/100");
    for (std::size_t k = 0; k < kKinds; ++k) {
        const double n = static_cast<double>(hands[k]);
        std::printf("%-8s %7ld %5.1f%% %5.1f%% %6.2f %6.1f%% %8.1f\n",
                    std::string(bot_kind_name(kRandomPoolKinds[k])).c_str(), hands[k], 100.0 * static_cast<double>(vpip[k]) / n,
                    100.0 * static_cast<double>(pfr[k]) / n,
                    static_cast<double>(post_aggr[k]) / std::max(1.0, static_cast<double>(post_calls[k])),
                    100.0 * static_cast<double>(post_folds[k]) / std::max(1.0, static_cast<double>(post_decisions[k])),
                    net[k] / n / 20.0 * 100.0);
    }
}

// --- All-in and heads-up behaviour -------------------------------------------

TEST(AllInBehaviour, GtoCallsAPriceWithEnoughEquityAndFoldsWithout) {
    const std::array<Card, 5> board = board_of("Kd", "8s", "3c", "2h", "Jd");
    GameEngine good = heads_up_to_flop(hole("Ks", "Kh"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(good.apply_action({ActionType::AllIn, 0}));
    GtoBot gto(29, kScenarioRollouts);
    const Action with_set = gto.decide(good.player_view(0));
    EXPECT_TRUE(with_set.type == ActionType::Call || is_aggressive(with_set));

    GameEngine bad = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ad"), board);
    ASSERT_TRUE(bad.apply_action({ActionType::AllIn, 0}));
    EXPECT_EQ(gto.decide(bad.player_view(0)).type, ActionType::Fold);
}

TEST(HeadsUp, GtoOpensWideOnTheButton) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Qc", "6d");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(should_open(HandClass::from_cards(*(deal.holes[0]->begin()), (*deal.holes[0])[1]), Position::BTN, 2));
    GtoBot gto(30);
    EXPECT_EQ(gto.decide(engine.player_view(0)).type, ActionType::Raise);
}

}  // namespace
