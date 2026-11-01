#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bots/bots.h"
#include "engine/game_engine.h"
#include "logging/hand_logger.h"

using namespace poker;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }

// Pocket pairs on a board that pairs nothing: aces beat kings beat queens.
ScriptedDeal three_handed_deal() {
    ScriptedDeal deal;
    deal.board = {card("2c"), card("7d"), card("9h"), card("Js"), card("3c")};
    deal.holes[0] = hole("As", "Ad");
    deal.holes[1] = hole("Ks", "Kd");
    deal.holes[2] = hole("Qs", "Qd");
    return deal;
}

GameConfig config_with(std::array<int, kMaxSeats> stacks, std::optional<int> hero = 0) {
    GameConfig config;
    config.stacks = stacks;
    config.first_button = 0;
    config.hero_seat = hero;
    return config;
}

constexpr std::array<int, kMaxSeats> kThreeHanded{1000, 1000, 1000, 0, 0, 0};

HandLogContext context(std::optional<int> hero = 0) {
    HandLogContext c;
    c.session_id = "S1";
    c.small_blind = 10;
    c.big_blind = 20;
    c.seat_labels = {"Hero", "Nit", "Shift:GTO", "Loose", "WildGTO", "GTO"};
    c.hero_seat = hero;
    return c;
}

void act(GameEngine& e, ActionType type, int amount = 0) { ASSERT_TRUE(e.apply_action({type, amount})); }

json parse(const GameEngine& e, const HandLogContext& c) { return json::parse(serialize_hand(e.state(), c)); }

std::vector<std::string> read_lines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// A directory unique to the running test, removed afterwards.
class LoggerFiles : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("poker_logger_") + info->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }
    fs::path dir_;
};

// A hint with chosen EVs, for testing mistake rules apart from the advisor's model.
HintRecommendation sample_hint(Action action, const char* text) {
    HintRecommendation hint;
    hint.action = action;
    hint.best_action = text;
    hint.equity_pct = 62.34;
    hint.pot_odds_pct = 25.0;
    hint.mdf_pct = 66.67;
    hint.rationale = "On the button with strong equity.";
    return hint;
}

PlayerView view_facing_open(GameEngine& e) {
    e.start_hand(three_handed_deal());  // Seat 0 is the button, first to act
    return e.player_view(0);
}

}  // namespace

// --- Serialization -----------------------------------------------------------

TEST(HandLog, ExactLineForAFullHand) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Raise, 60);  // BTN
    act(e, ActionType::Fold);       // SB
    act(e, ActionType::Call);       // BB
    act(e, ActionType::Check);
    act(e, ActionType::Bet, 100);
    act(e, ActionType::Call);
    for (int i = 0; i < 4; ++i) act(e, ActionType::Check);

    const std::string hand = describe_hand(*e.state().last_result->showdown_ranks[0]);
    const std::string expected =
        R"({"session_id":"S1","h":1,"sb":10,"bb":20,"btn":0,"post":{"sb":{"s":1,"amt":10},"bb":{"s":2,"amt":20}},)"
        R"("seats":[{"s":0,"p":"Hero","pos":"BTN","st":1000,"c":"AsAd"},{"s":1,"p":"Nit","pos":"SB","st":1000},)"
        R"({"s":2,"p":"Shift:GTO","pos":"BB","st":1000,"c":"QsQd"}],)"
        R"("pre":{"pot":30,"a":["BTN:R60","SB:F","BB:C40"]},)"
        R"("flop":{"b":"2c7d9h","pot":130,"a":["BB:X","BTN:B100","BB:C100"]},)"
        R"("turn":{"b":"Js","pot":330,"a":["BB:X","BTN:X"]},)"
        R"("riv":{"b":"3c","pot":330,"a":["BB:X","BTN:X"]},)"
        R"("win":[{"p":"Hero","amt":330,"hand":")" + hand + R"("}],"hero_mistakes":[]})";
    EXPECT_EQ(serialize_hand(e.state(), context()), expected);
}

TEST(HandLog, RejectsAHandThatHasNotFinished) {
    GameEngine e(config_with(kThreeHanded));
    EXPECT_THROW(serialize_hand(e.state(), context()), std::logic_error);
    e.start_hand();
    EXPECT_THROW(serialize_hand(e.state(), context()), std::logic_error);
}

TEST(HandLog, ShortBlindsLogWhatWasPostedNotTheConfiguredSize) {
    GameEngine e(config_with({1000, 4, 7, 0, 0, 0}));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Call);

    const json line = parse(e, context());
    EXPECT_EQ(line["sb"], 10);
    EXPECT_EQ(line["bb"], 20);
    EXPECT_EQ(line["post"]["sb"]["s"], 1);
    EXPECT_EQ(line["post"]["sb"]["amt"], 4);
    EXPECT_EQ(line["post"]["bb"]["s"], 2);
    EXPECT_EQ(line["post"]["bb"]["amt"], 7);
    EXPECT_EQ(line["seats"][1]["st"], 4);
    EXPECT_EQ(line["seats"][2]["st"], 7);
}

TEST(HandLog, BlindsAreLoggedOnlyInPostNotInTheActionList) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    const json line = parse(e, context());
    EXPECT_EQ(line["pre"]["pot"], 30);
    EXPECT_EQ(line["pre"]["a"], json::array({"BTN:F", "SB:F"}));
}

TEST(HandLog, HeadsUpButtonPostsTheSmallBlind) {
    GameEngine e(config_with({1000, 1000, 0, 0, 0, 0}));
    e.start_hand();
    act(e, ActionType::Fold);
    const json line = parse(e, context());
    EXPECT_EQ(line["post"]["sb"]["s"], 0);
    EXPECT_EQ(line["seats"][0]["pos"], "BTN");
    EXPECT_EQ(line["seats"][1]["pos"], "BB");
}

TEST(HandLog, ActionCodesCoverEveryTypeAndAmountRule) {
    GameEngine e(config_with({1000, 1000, 1000, 0, 0, 0}));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Raise, 60);  // Total for the street
    act(e, ActionType::Call);       // Chips added: 50 on top of the small blind
    act(e, ActionType::Call);       // Chips added: 40 on top of the big blind
    act(e, ActionType::Check);
    act(e, ActionType::Bet, 50);
    act(e, ActionType::AllIn);  // Total for the street
    act(e, ActionType::Fold);
    act(e, ActionType::AllIn);  // Calling for the rest also shows as A

    const json line = parse(e, context());
    EXPECT_EQ(line["pre"]["a"], json::array({"BTN:R60", "SB:C50", "BB:C40"}));
    EXPECT_EQ(line["flop"]["a"], json::array({"SB:X", "BB:B50", "BTN:A940", "SB:F", "BB:A940"}));
}

TEST(HandLog, AllInRunoutListsStreetsWithEmptyActionsAndARefund) {
    GameEngine e(config_with({500, 300, 0, 0, 0, 0}));
    e.start_hand(three_handed_deal());
    act(e, ActionType::AllIn);
    act(e, ActionType::Call);

    const json line = parse(e, context());
    EXPECT_EQ(line["pre"]["a"], json::array({"BTN:A500", "BB:A300"}));
    EXPECT_EQ(line["flop"]["b"], "2c7d9h");
    EXPECT_EQ(line["flop"]["a"], json::array());
    EXPECT_EQ(line["riv"]["b"], "3c");
    EXPECT_EQ(line["riv"]["pot"], 800);
    EXPECT_EQ(line["ref"]["s"], 0);
    EXPECT_EQ(line["ref"]["amt"], 200);
    EXPECT_EQ(line["win"][0]["amt"], 600);
    EXPECT_EQ(line["win"][0]["p"], "Hero");
    EXPECT_EQ(line["seats"][0]["st"], 500);
    EXPECT_EQ(line["seats"][1]["st"], 300);
}

TEST(HandLog, HandEndingOnTheFlopOmitsLaterStreetsAndHandName) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Call);
    act(e, ActionType::Call);
    act(e, ActionType::Check);
    act(e, ActionType::Bet, 40);
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);

    const json line = parse(e, context());
    EXPECT_TRUE(line.contains("flop"));
    EXPECT_FALSE(line.contains("turn"));
    EXPECT_FALSE(line.contains("riv"));
    EXPECT_EQ(line["win"][0]["p"], "Nit");
    EXPECT_FALSE(line["win"][0].contains("hand"));
}

TEST(HandLog, SplitPotListsEachWinner) {
    GameEngine e(config_with(kThreeHanded));
    ScriptedDeal deal;
    deal.board = {card("As"), card("Ks"), card("Qd"), card("Jc"), card("Th")};
    deal.holes[0] = hole("2c", "3d");
    deal.holes[1] = hole("4h", "5c");
    deal.holes[2] = hole("6h", "7c");
    e.start_hand(deal);
    act(e, ActionType::Call);
    act(e, ActionType::Fold);
    act(e, ActionType::Check);
    for (int i = 0; i < 6; ++i) act(e, ActionType::Check);

    const json line = parse(e, context());
    ASSERT_EQ(line["win"].size(), 2u);
    EXPECT_EQ(line["win"][0]["amt"].get<int>() + line["win"][1]["amt"].get<int>(), 50);
    EXPECT_TRUE(line["win"][0].contains("hand"));
}

// --- Which hole cards are shown ----------------------------------------------

TEST(HandLog, CardsShownForFlopSeersAndHeroOnly) {
    GameEngine e(config_with(kThreeHanded, 1));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);  // Hero, the small blind, folds preflop; nobody sees a flop

    const json line = parse(e, context(1));
    EXPECT_FALSE(line.contains("flop"));
    EXPECT_FALSE(line["seats"][0].contains("c"));
    EXPECT_EQ(line["seats"][1]["c"], "KsKd");  // Hero, needed to read mistakes
    EXPECT_FALSE(line["seats"][2].contains("c"));
}

TEST(HandLog, FoldOnTheFlopStillShowsCardsAndPreflopFoldDoesNot) {
    GameEngine e(config_with({1000, 1000, 1000, 1000, 0, 0}, std::nullopt));
    ScriptedDeal deal = three_handed_deal();
    deal.holes[3] = hole("8s", "8d");
    e.start_hand(deal);
    act(e, ActionType::Fold);  // UTG (seat 3) preflop
    act(e, ActionType::Call);
    act(e, ActionType::Call);
    act(e, ActionType::Check);
    act(e, ActionType::Bet, 40);
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);

    const json line = parse(e, context(std::nullopt));
    EXPECT_FALSE(line["seats"][3].contains("c"));
    EXPECT_EQ(line["seats"][0]["c"], "AsAd");
    EXPECT_EQ(line["seats"][2]["c"], "QsQd");
}

TEST(HandLog, BustedSeatsAreLeftOutButSeatIndexesStayTrue) {
    GameEngine e(config_with({1000, 0, 1000, 0, 1000, 0}));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    const json line = parse(e, context());
    ASSERT_EQ(line["seats"].size(), 3u);
    EXPECT_EQ(line["seats"][1]["s"], 2);
    EXPECT_EQ(line["seats"][2]["s"], 4);
}

TEST(HandLog, ShifterSubProfileAppearsInSeatAndWinLabels) {
    GameEngine e(config_with({1000, 1000, 1000, 0, 0, 0}, std::nullopt));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    const json line = parse(e, context(std::nullopt));
    EXPECT_EQ(line["seats"][2]["p"], "Shift:GTO");
    EXPECT_EQ(line["win"][0]["p"], "Shift:GTO");
}

// --- Mistake tracking --------------------------------------------------------

TEST(Mistakes, FollowingTheHintIsNotAMistake) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");
    hint.ev.fold = 0.0;
    hint.ev.check_call = 100.0;
    EXPECT_FALSE(assess_hero_decision(view, hint, {ActionType::Call}));
}

TEST(Mistakes, EvGapAtThresholdCountsAndJustBelowDoesNot) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");
    hint.ev.fold = 0.0;

    hint.ev.check_call = 19.9;  // Big blind is 20
    EXPECT_FALSE(assess_hero_decision(view, hint, {ActionType::Fold}));

    hint.ev.check_call = 20.0;
    const auto mistake = assess_hero_decision(view, hint, {ActionType::Fold});
    ASSERT_TRUE(mistake);
    EXPECT_DOUBLE_EQ(mistake->ev_loss, 20.0);
}

TEST(Mistakes, RecordsEverythingNeededToInspectTheDecision) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Raise, 60}, "RAISE to 60 (75% Pot)");
    hint.ev.fold = 0.0;
    hint.ev.raise_75 = 80.0;
    hint.raise_75_to = 60;

    const auto mistake = assess_hero_decision(view, hint, {ActionType::Fold});
    ASSERT_TRUE(mistake);
    EXPECT_EQ(mistake->street, Street::Preflop);
    EXPECT_EQ(mistake->taken, "F");
    EXPECT_EQ(mistake->recommended, "RAISE to 60 (75% Pot)");
    EXPECT_DOUBLE_EQ(mistake->equity_pct, 62.34);
    EXPECT_DOUBLE_EQ(mistake->pot_odds_pct, 25.0);
    EXPECT_DOUBLE_EQ(mistake->mdf_pct, 66.67);
    EXPECT_DOUBLE_EQ(mistake->ev_loss, 80.0);
    EXPECT_EQ(mistake->rationale, "On the button with strong equity.");
}

TEST(Mistakes, SizeDeviationCountsOnlyWhenTheNearestOptionLosesEnough) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Raise, 60}, "RAISE to 60");
    hint.raise_33_to = 40;
    hint.raise_75_to = 60;
    hint.ev.raise_33 = 50.0;
    hint.ev.raise_75 = 60.0;
    hint.ev.all_in = -200.0;

    EXPECT_FALSE(assess_hero_decision(view, hint, {ActionType::Raise, 45}));  // Nearest is 33%: 10 lost

    const auto shove = assess_hero_decision(view, hint, {ActionType::AllIn});
    ASSERT_TRUE(shove);
    EXPECT_DOUBLE_EQ(shove->ev_loss, 260.0);
    EXPECT_EQ(shove->taken, "A1000");

    const auto huge = assess_hero_decision(view, hint, {ActionType::Raise, 900});  // Nearest is all-in
    ASSERT_TRUE(huge);
    EXPECT_EQ(huge->taken, "R900");
}

TEST(Mistakes, FoldingWhenCheckingIsFreeAlwaysCounts) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Call);
    act(e, ActionType::Call);
    const PlayerView view = e.player_view(2);  // Big blind's option
    ASSERT_TRUE(view.legal.allows(ActionType::Check));

    HintRecommendation hint = sample_hint({ActionType::Check}, "CHECK");
    hint.ev.fold = 0.0;
    hint.ev.check_call = 1.0;  // Far below the EV threshold
    const auto mistake = assess_hero_decision(view, hint, {ActionType::Fold});
    ASSERT_TRUE(mistake);
    EXPECT_EQ(mistake->taken, "F");
}

TEST(Mistakes, MissingEvComparesActionTypesInstead) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");

    EXPECT_FALSE(assess_hero_decision(view, hint, {ActionType::Call}));
    const auto mistake = assess_hero_decision(view, hint, {ActionType::Raise, 60});
    ASSERT_TRUE(mistake);
    EXPECT_DOUBLE_EQ(mistake->ev_loss, 0.0);
}

TEST(Mistakes, AllInForLessThanTheCallIsPricedAsACall) {
    GameEngine e(config_with({1000, 1000, 50, 0, 0, 0}, 2));
    e.start_hand(three_handed_deal());
    act(e, ActionType::Raise, 200);
    act(e, ActionType::Fold);
    const PlayerView view = e.player_view(2);
    ASSERT_FALSE(view.legal.allows(ActionType::Raise));

    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 30");
    hint.ev.check_call = 40.0;
    EXPECT_FALSE(assess_hero_decision(view, hint, {ActionType::AllIn}));

    HintRecommendation fold_hint = sample_hint({ActionType::Fold}, "FOLD");
    fold_hint.ev.fold = 0.0;
    fold_hint.ev.check_call = -30.0;
    const auto mistake = assess_hero_decision(view, fold_hint, {ActionType::Call});
    ASSERT_TRUE(mistake);
    EXPECT_EQ(mistake->taken, "A50");  // An all-in call logs as A, like the action list
}

TEST(Mistakes, RealAdvisorFlagsFoldingAcesPreflop) {
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    const TrainingAdvisor advisor(1, 2000);
    const HintRecommendation hint = advisor.analyze(view);

    const auto mistake = assess_hero_decision(view, hint, {ActionType::Fold});
    ASSERT_TRUE(mistake);
    EXPECT_EQ(mistake->recommended, hint.best_action);
    EXPECT_EQ(mistake->rationale, hint.rationale);
    EXPECT_FALSE(mistake->rationale.empty());
    EXPECT_GT(mistake->equity_pct, 70.0);
    EXPECT_GT(mistake->ev_loss, 20.0);

    EXPECT_FALSE(assess_hero_decision(view, hint, hint.action));
}

TEST(Mistakes, SerializedWithEveryFieldAndRoundedNumbers) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);

    HandLogContext c = context();
    c.hero_mistakes.push_back({Street::Turn, "C40", "FOLD", 18.26, 33.333, 66.667, 12.34, "Too \"thin\"\non the turn."});
    const std::string text = serialize_hand(e.state(), c);
    EXPECT_EQ(text.find('\n'), std::string::npos);

    const json mistake = json::parse(text)["hero_mistakes"][0];
    EXPECT_EQ(mistake["st"], "turn");
    EXPECT_EQ(mistake["did"], "C40");
    EXPECT_EQ(mistake["rec"], "FOLD");
    EXPECT_DOUBLE_EQ(mistake["eq"].get<double>(), 18.3);
    EXPECT_DOUBLE_EQ(mistake["odds"].get<double>(), 33.3);
    EXPECT_DOUBLE_EQ(mistake["mdf"].get<double>(), 66.7);
    EXPECT_DOUBLE_EQ(mistake["ev_loss"].get<double>(), 12.3);
    EXPECT_EQ(mistake["why"], "Too \"thin\"\non the turn.");
}

TEST(Mistakes, InvalidUtf8InRationaleCannotBreakTheLog) {
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    HandLogContext c = context();
    c.hero_mistakes.push_back({Street::Preflop, "F", "CALL", 0, 0, 0, 0, std::string("bad \xC3\x28 byte")});
    EXPECT_NO_THROW((void)json::parse(serialize_hand(e.state(), c)));
}

// --- Files -------------------------------------------------------------------

TEST_F(LoggerFiles, CreatesDirectoriesAndAppendsOneLinePerHand) {
    const fs::path path = dir_ / "nested" / "hand_history.jsonl";
    {
        HandLogger logger(path);
        GameEngine e(config_with(kThreeHanded));
        for (int hand = 1; hand <= 3; ++hand) {
            e.start_hand();
            act(e, ActionType::Fold);
            act(e, ActionType::Fold);
            logger.write_hand(e.state(), context());
        }
    }
    {
        HandLogger reopened(path);  // Sessions accumulate in one file
        GameEngine e(config_with(kThreeHanded));
        e.start_hand();
        act(e, ActionType::Fold);
        act(e, ActionType::Fold);
        reopened.write_hand(e.state(), context());
    }

    const auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(json::parse(lines[2])["h"], 3);
    EXPECT_EQ(json::parse(lines[3])["h"], 1);
    for (const std::string& line : lines) EXPECT_EQ(line.find('\r'), std::string::npos);
}

TEST_F(LoggerFiles, LinesAreOnDiskBeforeTheLoggerCloses) {
    const fs::path path = dir_ / "h.jsonl";
    HandLogger logger(path);
    GameEngine e(config_with(kThreeHanded));
    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    logger.write_hand(e.state(), context());
    EXPECT_EQ(read_lines(path).size(), 1u);
}

TEST_F(LoggerFiles, UnopenablePathThrows) {
    fs::create_directories(dir_ / "taken");
    EXPECT_THROW(HandLogger(dir_ / "taken"), std::runtime_error);
    EXPECT_THROW(SessionLogger({}, dir_ / "taken"), std::runtime_error);
}

TEST_F(LoggerFiles, PendingMistakesGoInTheirHandAndAreThenCleared) {
    const fs::path path = dir_ / "h.jsonl";
    HandLogger logger(path);
    GameEngine e(config_with(kThreeHanded));

    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");
    hint.ev.fold = 0.0;
    hint.ev.check_call = 200.0;
    ASSERT_TRUE(logger.record_hero_decision(view, hint, {ActionType::Fold}));
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);

    HandLogContext stale = context();
    stale.hero_mistakes.push_back({});  // write_hand uses the noted ones instead
    const auto written = logger.write_hand(e.state(), stale);
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0].taken, "F");

    e.start_hand();
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    EXPECT_TRUE(logger.write_hand(e.state(), context()).empty());

    const auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), 2u);
    const json first = json::parse(lines[0]);
    ASSERT_EQ(first["hero_mistakes"].size(), 1u);
    EXPECT_EQ(first["hero_mistakes"][0]["rec"], "CALL 20");
    EXPECT_EQ(first["hero_mistakes"][0]["why"], "On the button with strong equity.");
    EXPECT_DOUBLE_EQ(first["hero_mistakes"][0]["eq"].get<double>(), 62.3);
    EXPECT_DOUBLE_EQ(first["hero_mistakes"][0]["odds"].get<double>(), 25.0);
    EXPECT_TRUE(json::parse(lines[1])["hero_mistakes"].empty());
}

TEST_F(LoggerFiles, SeveralMistakesOnOneStreetAreKeptSeparately) {
    const fs::path path = dir_ / "h.jsonl";
    HandLogger logger(path);
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");
    hint.ev.fold = 0.0;
    hint.ev.check_call = 100.0;
    logger.record_hero_decision(view, hint, {ActionType::Fold});
    logger.record_hero_decision(view, hint, {ActionType::Fold});
    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    EXPECT_EQ(logger.write_hand(e.state(), context()).size(), 2u);
}

// --- Session summaries -------------------------------------------------------

TEST(SessionId, IsCompactUtcTimeWithRandomSuffixAndDistinct) {
    const std::string id = make_session_id();
    ASSERT_EQ(id.size(), 21u) << id;  // YYYYMMDDTHHMMSSZ-xxxx
    EXPECT_EQ(id[8], 'T');
    EXPECT_EQ(id[15], 'Z');
    EXPECT_EQ(id[16], '-');

    std::set<std::string> ids;
    for (int i = 0; i < 200; ++i) ids.insert(make_session_id());
    EXPECT_GT(ids.size(), 190u);  // Same second: only the suffix differs, and may rarely clash
}

TEST_F(LoggerFiles, HandsJoinToTheirSessionAcrossAppendedRuns) {
    const fs::path hands = dir_ / "h.jsonl";
    const fs::path sessions = dir_ / "s.jsonl";
    for (const char* id : {"run-a", "run-b"}) {
        HandLogger hand_logger(hands);
        SessionLogger session_logger({1000, 1000, 1000, 0, 0, 0}, sessions);
        HandLogContext c = context();
        c.session_id = id;
        GameEngine e(config_with(kThreeHanded));
        e.start_hand();
        act(e, ActionType::Fold);
        act(e, ActionType::Fold);
        hand_logger.write_hand(e.state(), c);
        session_logger.write_session(e.session_summary(), c, "t");
    }

    const auto hand_lines = read_lines(hands);
    const auto session_lines = read_lines(sessions);
    ASSERT_EQ(hand_lines.size(), 2u);
    ASSERT_EQ(session_lines.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(json::parse(hand_lines[i])["session_id"], json::parse(session_lines[i])["session_id"]);
        EXPECT_EQ(json::parse(hand_lines[i])["h"], 1);  // h alone could not tell them apart
    }
    EXPECT_NE(json::parse(hand_lines[0])["session_id"], json::parse(hand_lines[1])["session_id"]);
}

TEST_F(LoggerFiles, DiscardedMistakesDoNotReachTheNextHand) {
    const fs::path path = dir_ / "h.jsonl";
    HandLogger logger(path);
    GameEngine e(config_with(kThreeHanded));
    const PlayerView view = view_facing_open(e);
    HintRecommendation hint = sample_hint({ActionType::Call}, "CALL 20");
    hint.ev.fold = 0.0;
    hint.ev.check_call = 200.0;
    ASSERT_TRUE(logger.record_hero_decision(view, hint, {ActionType::Fold}));
    logger.discard_pending_mistakes();

    act(e, ActionType::Fold);
    act(e, ActionType::Fold);
    EXPECT_TRUE(logger.write_hand(e.state(), context()).empty());
}

TEST_F(LoggerFiles, SessionSummaryExactLine) {
    const fs::path path = dir_ / "s.jsonl";
    SessionLogger logger({100, 1000, 1000, 0, 0, 0}, path);
    logger.record_hand_mistakes({{Street::Preflop, "F", "CALL", 0, 0, 0, 12.34, ""},
                                 {Street::Preflop, "C20", "FOLD", 0, 0, 0, 1.0, ""},
                                 {Street::River, "X", "BET", 0, 0, 0, 5.0, ""}});

    SessionSummary summary;
    summary.hands_played = 7;
    summary.final_stacks = {0, 900, 1200, 0, 0, 0};
    summary.busted_on_hand[0] = 7;
    summary.hero_busted = true;

    logger.write_session(summary, context(), "2026-11-01T12:00:00Z");
    const auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              R"({"session_id":"S1","ts":"2026-11-01T12:00:00Z","sb":10,"bb":20,"hands":7,"done":true,)"
              R"("seats":[{"s":0,"p":"Hero","st":100,"end":0,"bust":7},{"s":1,"p":"Nit","st":1000,"end":900},)"
              R"({"s":2,"p":"Shift:GTO","st":1000,"end":1200}],)"
              R"("hero":{"s":0,"net":-100,"bust":true,"mistakes":3,"ev_loss":18.3,)"
              R"("by_street":{"pre":2,"flop":0,"turn":0,"riv":1}}})");
}

TEST_F(LoggerFiles, SessionReportsWinnerAndNoHeroBlockWithoutAHero) {
    const fs::path path = dir_ / "s.jsonl";
    SessionLogger logger({1000, 1000, 0, 0, 0, 0}, path);
    SessionSummary summary;
    summary.hands_played = 2;
    summary.final_stacks = {2000, 0, 0, 0, 0, 0};
    summary.winner = 0;
    logger.write_session(summary, context(std::nullopt));

    const json line = json::parse(read_lines(path)[0]);
    EXPECT_TRUE(line["done"]);
    EXPECT_EQ(line["winner"], "Hero");
    EXPECT_FALSE(line.contains("hero"));
    const std::string ts = line["ts"];
    EXPECT_EQ(ts.size(), 20u);  // e.g. 2026-11-01T12:00:00Z
    EXPECT_EQ(ts.back(), 'Z');
}

TEST_F(LoggerFiles, UnfinishedSessionIsMarkedNotDone) {
    const fs::path path = dir_ / "s.jsonl";
    SessionLogger logger({1000, 1000, 0, 0, 0, 0}, path);
    SessionSummary summary;
    summary.final_stacks = {1000, 1000, 0, 0, 0, 0};
    logger.write_session(summary, context(), "t");
    EXPECT_FALSE(json::parse(read_lines(path)[0])["done"]);
}

// --- End to end --------------------------------------------------------------

// A real session: bots against a Hero who ignores the advisor every third decision.
// Each logged line has to account for its chips, against the engine and the next line.
TEST_F(LoggerFiles, FullSessionProducesConsistentLogs) {
    GameConfig config;
    config.seed = 77;
    config.hero_seat = 0;
    config.first_button = 0;
    GameEngine e(config);

    std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots;
    for (int seat = 1; seat < kMaxSeats; ++seat) {
        bots[seat] = make_bot(kAllBotKinds[seat - 1], 1000 + seat, 100);
    }
    const TrainingAdvisor advisor(9, 200);

    const fs::path hands_path = dir_ / "h.jsonl";
    const fs::path session_path = dir_ / "s.jsonl";
    HandLogger hands(hands_path);
    std::array<int, kMaxSeats> starting{};
    for (int seat = 0; seat < kMaxSeats; ++seat) starting[seat] = e.state().seats[seat].stack;
    SessionLogger session(starting, session_path);

    HandLogContext c;
    c.session_id = make_session_id();
    c.small_blind = 10;
    c.big_blind = 20;
    c.hero_seat = 0;
    c.seat_labels[0] = "Hero";

    std::array<int, kMaxSeats> expected_start = starting;
    int decisions = 0;
    int total_mistakes = 0;
    bool saw_shifter_label = false;

    for (int hand = 0; hand < 60 && e.state().phase == GamePhase::BetweenHands; ++hand) {
        e.start_hand();
        for (int seat = 1; seat < kMaxSeats; ++seat) bots[seat]->begin_hand(e.state().hand_number);

        while (e.state().phase == GamePhase::Betting) {
            const int seat = *e.state().to_act;
            const PlayerView view = e.player_view(seat);
            Action action;
            if (seat == 0) {
                const HintRecommendation hint = advisor.analyze(view);
                ++decisions;
                action = decisions % 3 == 0 ? Action{ActionType::Fold} : hint.action;
                hands.record_hero_decision(view, hint, action);
            } else {
                action = bots[seat]->decide(view);
            }
            ASSERT_TRUE(e.apply_action(action));
        }

        for (int seat = 1; seat < kMaxSeats; ++seat) c.seat_labels[seat] = bots[seat]->hand_log_label();
        const auto mistakes = hands.write_hand(e.state(), c);
        session.record_hand_mistakes(mistakes);
        total_mistakes += static_cast<int>(mistakes.size());

        const std::string last_line = read_lines(hands_path).back();
        const json line = json::parse(last_line);
        EXPECT_EQ(line["h"], e.state().hand_number);
        EXPECT_EQ(line["session_id"], c.session_id);

        for (const json& seat : line["seats"]) {
            const int index = seat["s"];
            EXPECT_EQ(seat["st"].get<int>(), expected_start[index]) << last_line;
            const std::string label = seat["p"];
            if (label.rfind("Shift:", 0) == 0) saw_shifter_label = true;
        }
        for (int seat = 0; seat < kMaxSeats; ++seat) expected_start[seat] = e.state().seats[seat].stack;

        int pot_paid = line.contains("ref") ? line["ref"]["amt"].get<int>() : 0;
        for (const json& win : line["win"]) pot_paid += win["amt"].get<int>();
        EXPECT_EQ(pot_paid, e.state().total_pot()) << last_line;
    }

    ASSERT_GT(decisions, 0);
    EXPECT_GT(total_mistakes, 0);
    EXPECT_TRUE(saw_shifter_label);

    c.seat_labels[5] = std::string(bots[5]->name());
    session.write_session(e.session_summary(), c);
    const json summary = json::parse(read_lines(session_path).at(0));
    EXPECT_EQ(summary["session_id"], c.session_id);
    EXPECT_EQ(summary["hero"]["mistakes"], total_mistakes);
    EXPECT_EQ(summary["hands"], e.state().hand_number);
    EXPECT_EQ(summary["seats"][5]["p"], "Shifter");
    int chips = 0;
    for (const json& seat : summary["seats"]) chips += seat["end"].get<int>();
    EXPECT_EQ(chips, 12000);
}
