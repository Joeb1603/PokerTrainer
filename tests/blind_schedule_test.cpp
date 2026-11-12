#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "controller/blind_schedule.h"

using namespace poker;

namespace {

std::vector<int> big_blinds(const std::vector<BlindLevel>& levels) {
    std::vector<int> out;
    for (const BlindLevel& level : levels) out.push_back(level.big_blind);
    return out;
}

std::vector<int> small_blinds(const std::vector<BlindLevel>& levels) {
    std::vector<int> out;
    for (const BlindLevel& level : levels) out.push_back(level.small_blind);
    return out;
}

std::vector<BlindLevel> three_levels() {
    return {{10, 20, 0, AnteType::None}, {15, 30, 0, AnteType::None}, {20, 40, 40, AnteType::BigBlind}};
}

}  // namespace

TEST(BlindSchedule, TidyChipsRoundsToDealerAmounts) {
    EXPECT_EQ(tidy_chips(0.4), 1);
    EXPECT_EQ(tidy_chips(7.4), 7);
    EXPECT_EQ(tidy_chips(12), 10);
    EXPECT_EQ(tidy_chips(13), 15);
    EXPECT_EQ(tidy_chips(98), 100);
    EXPECT_EQ(tidy_chips(120), 125);
    EXPECT_EQ(tidy_chips(160), 150);
    EXPECT_EQ(tidy_chips(1200), 1250);
    EXPECT_EQ(tidy_chips(9999), 10000);
    EXPECT_EQ(tidy_chips(13000), 12500);
}

TEST(BlindSchedule, SmallBlindIsHalfRoundedDownToATidyAmount) {
    EXPECT_EQ(small_blind_for(20), 10);  // Today's table
    EXPECT_EQ(small_blind_for(2), 1);
    EXPECT_EQ(small_blind_for(3), 1);
    EXPECT_EQ(small_blind_for(25), 10);
    EXPECT_EQ(small_blind_for(125), 60);
    EXPECT_EQ(small_blind_for(250), 125);
    EXPECT_EQ(small_blind_for(1250), 625);
}

TEST(BlindSchedule, StandardPresetFromTwenty) {
    const auto levels = preset_schedule(BlindPreset::Standard, 20, AnteType::BigBlind, 4);
    EXPECT_EQ(big_blinds(levels), (std::vector<int>{20, 30, 40, 60, 80, 100, 125, 150, 200, 250, 300, 400, 500, 600,
                                                    800, 1000, 1250, 1500, 2000}));
    EXPECT_EQ(small_blinds(levels), (std::vector<int>{10, 15, 20, 30, 40, 50, 60, 75, 100, 125, 150, 200, 250, 300,
                                                      400, 500, 625, 750, 1000}));
    EXPECT_EQ(levels[2].ante, 0);
    EXPECT_EQ(levels[2].ante_type, AnteType::None);
    EXPECT_EQ(levels[3].ante, 60);  // Level 4: a big-blind ante of one big blind
    EXPECT_EQ(levels[3].ante_type, AnteType::BigBlind);
}

TEST(BlindSchedule, TurboAndHyperPresets) {
    EXPECT_EQ(big_blinds(preset_schedule(BlindPreset::Turbo, 20, AnteType::None, 4)),
              (std::vector<int>{20, 40, 60, 80, 125, 150, 250, 325, 500, 700, 1000, 1500, 2000}));
    EXPECT_EQ(big_blinds(preset_schedule(BlindPreset::Hyper, 20, AnteType::None, 4)),
              (std::vector<int>{20, 40, 80, 125, 200, 300, 500, 800, 1250, 2000}));
}

TEST(BlindSchedule, PresetsRiseStrictlyEvenFromTinyBlinds) {
    for (const BlindPreset preset : kBlindPresets) {
        for (const int bb : {2, 3, 5, 7, 20, 25, 50, 100, 10000}) {
            const auto levels = preset_schedule(preset, bb, AnteType::PerPlayer, 1);
            ASSERT_EQ(levels.front().big_blind, bb);
            for (std::size_t i = 1; i < levels.size(); ++i) {
                EXPECT_GT(levels[i].big_blind, levels[i - 1].big_blind) << to_string(preset) << " " << bb;
            }
            GameSetup setup = default_tournament();
            setup.preset = preset;
            setup.big_blind = std::max(bb, kMinBigBlind);
            EXPECT_FALSE(validate(setup)) << *validate(setup);
        }
    }
}

TEST(BlindSchedule, PerPlayerAntesAreAnEighthOfTheBigBlind) {
    const auto levels = preset_schedule(BlindPreset::Standard, 20, AnteType::PerPlayer, 4);
    EXPECT_EQ(levels[3].ante, 8);   // 60 / 8 = 7.5
    EXPECT_EQ(levels[4].ante, 10);  // 80 / 8
    EXPECT_EQ(levels[6].ante, 15);  // 125 / 8 = 15.6
    EXPECT_EQ(levels[3].ante_type, AnteType::PerPlayer);
    const auto none = with_antes(levels, AnteType::None, 1);
    for (const BlindLevel& level : none) {
        EXPECT_EQ(level.ante, 0);
        EXPECT_EQ(level.ante_type, AnteType::None);
    }
    const auto early = with_antes(levels, AnteType::BigBlind, 1);
    EXPECT_EQ(early[0].ante, 20);
}

TEST(BlindSchedule, ScalingKeepsShapeAndTidiness) {
    const auto doubled = scale_schedule(three_levels(), 2.0);
    EXPECT_EQ(big_blinds(doubled), (std::vector<int>{40, 60, 80}));
    EXPECT_EQ(small_blinds(doubled), (std::vector<int>{20, 30, 40}));
    EXPECT_EQ(doubled[2].ante, 80);
    EXPECT_EQ(doubled[2].ante_type, AnteType::BigBlind);

    // A small factor would merge levels; they stay strictly rising.
    const auto squashed = scale_schedule(three_levels(), 0.1);
    EXPECT_EQ(big_blinds(squashed), (std::vector<int>{2, 3, 4}));
    EXPECT_EQ(small_blinds(squashed), (std::vector<int>{1, 1, 2}));

    // An edited small blind keeps its share of the big blind.
    const auto third = scale_schedule({{5, 20, 0, AnteType::None}}, 3.0);
    EXPECT_EQ(third[0].big_blind, 60);
    EXPECT_EQ(third[0].small_blind, 15);
}

TEST(BlindSchedule, DefaultsAndFirstLevel) {
    const GameSetup freezeout = default_freezeout();
    EXPECT_EQ(freezeout.mode, GameMode::Freezeout);
    EXPECT_EQ(freezeout.first_level(), (BlindLevel{10, 20, 0, AnteType::None}));
    EXPECT_EQ(freezeout.starting_stack(), 2000);
    EXPECT_FALSE(validate(freezeout));

    const GameSetup tournament = default_tournament();
    EXPECT_EQ(tournament.first_level(), (BlindLevel{10, 20, 0, AnteType::None}));
    EXPECT_EQ(tournament.starting_stack(), 1000);
    EXPECT_EQ(tournament.schedule().size(), 19u);
    EXPECT_FALSE(validate(tournament));

    GameSetup edited = tournament;
    edited.levels = three_levels();
    edited.levels[0] = {25, 50, 0, AnteType::None};
    edited.levels[1] = {50, 100, 0, AnteType::None};
    edited.levels[2] = {75, 150, 0, AnteType::None};
    EXPECT_EQ(edited.first_level().big_blind, 50);
    EXPECT_EQ(edited.starting_stack(), 2500);
}

TEST(BlindSchedule, ValidationNamesTheProblem) {
    GameSetup setup = default_freezeout();
    setup.big_blind = 1;
    EXPECT_TRUE(validate(setup));
    setup.big_blind = 20;
    setup.starting_bb = 5;
    EXPECT_TRUE(validate(setup));

    GameSetup t = default_tournament();
    t.level_length = 0;
    EXPECT_TRUE(validate(t));
    t.level_length = 61;
    t.unit = LevelUnit::Minutes;
    EXPECT_TRUE(validate(t));
    t.level_length = 61;
    t.unit = LevelUnit::Hands;
    EXPECT_FALSE(validate(t));

    t = default_tournament();
    t.levels = three_levels();
    t.levels[1].big_blind = 10;
    EXPECT_NE(validate(t)->find("Level 2"), std::string::npos);
    t.levels = three_levels();
    t.levels[0].small_blind = 30;
    EXPECT_NE(validate(t)->find("Level 1"), std::string::npos);
    t.levels = three_levels();
    t.levels[2].ante = 100;
    EXPECT_NE(validate(t)->find("ante"), std::string::npos);
    t.levels = three_levels();
    t.levels[2].ante_type = AnteType::None;
    EXPECT_TRUE(validate(t));
    t.levels.assign(kMaxLevels + 1, BlindLevel{});
    EXPECT_TRUE(validate(t));
}

TEST(BlindSchedule, StartingStacksThatCouldOverflowAreRefused) {
    // Level 1 must equal the big blind, which is at most kMaxBigBlind, so the largest
    // stack is kMaxStartingBb * kMaxBigBlind; a huge level 1 is refused before that.
    static_assert(static_cast<long long>(kMaxStartingBb) * kMaxBigBlind <= kMaxStartingChips);
    GameSetup setup = default_tournament();
    setup.starting_bb = 500;
    setup.levels = {{2'500'000, 5'000'000, 0, AnteType::None}};  // 2.5e9 chips each
    EXPECT_TRUE(validate(setup));
    setup.big_blind = 5'000'000;
    EXPECT_NE(validate(setup)->find("Big blind must be"), std::string::npos);

    setup.big_blind = kMaxBigBlind;
    setup.levels = {{kMaxBigBlind / 2, kMaxBigBlind, 0, AnteType::None}};
    EXPECT_FALSE(validate(setup));
}

TEST(BlindSchedule, LevelOneMustBeTheBigBlind) {
    GameSetup setup = default_tournament();  // Big blind 20
    setup.levels = {{25, 50, 0, AnteType::None}, {50, 100, 0, AnteType::None}};
    EXPECT_EQ(validate(setup), "Level 1's big blind must equal the big blind.");
    setup.big_blind = 50;
    EXPECT_FALSE(validate(setup));

    // A freezeout ignores leftover levels; a preset is always in step.
    setup.mode = GameMode::Freezeout;
    setup.big_blind = 20;
    EXPECT_FALSE(validate(setup));
    setup.mode = GameMode::Tournament;
    setup.levels.clear();
    EXPECT_FALSE(validate(setup));
}

TEST(BlindSchedule, ScalingHugeLevelsSaturatesWithoutOverflow) {
    // Sixty levels at the cap, times ten: kept strictly rising (and refused), with no
    // int overflow on the way (run under UBSan in the sanitizer build).
    const std::vector<BlindLevel> huge(60, BlindLevel{kMaxLevelBigBlind / 2, kMaxLevelBigBlind, 0, AnteType::None});
    const std::vector<BlindLevel> scaled = scale_schedule(huge, 10.0);
    ASSERT_EQ(scaled.size(), 60u);
    EXPECT_EQ(scaled.front().big_blind, kMaxLevelBigBlind);
    for (std::size_t i = 1; i < scaled.size(); ++i) {
        EXPECT_GE(scaled[i].big_blind, scaled[i - 1].big_blind) << i;
        EXPECT_GE(scaled[i].small_blind, 1) << i;
        EXPECT_LE(scaled[i].small_blind, scaled[i].big_blind) << i;
    }
    EXPECT_EQ(scaled.back().big_blind, std::numeric_limits<int>::max());

    GameSetup setup = default_tournament();
    setup.levels.assign(scaled.begin(), scaled.begin() + 5);
    setup.big_blind = setup.levels.front().big_blind;
    EXPECT_TRUE(validate(setup));
}

TEST(BlindSchedule, FirstLevelMatchesTheSchedulesFirst) {
    for (const AnteType type : {AnteType::None, AnteType::PerPlayer, AnteType::BigBlind}) {
        for (const int from : {1, 4}) {
            GameSetup setup = default_tournament();
            setup.big_blind = 40;
            setup.ante_type = type;
            setup.ante_from_level = from;
            EXPECT_EQ(setup.first_level(), setup.schedule().front()) << to_string(type) << " " << from;
        }
    }
}

TEST(BlindSchedule, ParsesNames) {
    EXPECT_EQ(parse_game_mode("tournament"), GameMode::Tournament);
    EXPECT_EQ(parse_game_mode("Freezeout"), GameMode::Freezeout);
    EXPECT_FALSE(parse_game_mode("cash"));
    EXPECT_EQ(parse_blind_preset("turbo"), BlindPreset::Turbo);
    EXPECT_EQ(parse_blind_preset("Hyper"), BlindPreset::Hyper);
    EXPECT_FALSE(parse_blind_preset("hyp"));
    EXPECT_EQ(parse_level_unit("minutes"), LevelUnit::Minutes);
    EXPECT_EQ(parse_ante_type("bb"), AnteType::BigBlind);
    EXPECT_EQ(parse_ante_type("player"), AnteType::PerPlayer);
    EXPECT_EQ(parse_ante_type("none"), AnteType::None);
}

TEST(TournamentClock, LevelsByHands) {
    TournamentClock clock(three_levels(), LevelUnit::Hands, 2);
    EXPECT_EQ(clock.on_hand_started().big_blind, 20);
    EXPECT_FALSE(clock.level_changed());
    EXPECT_EQ(clock.hands_until_next_level(), 1);
    EXPECT_EQ(clock.on_hand_started().big_blind, 20);
    EXPECT_EQ(clock.hands_until_next_level(), 0);  // The next hand moves up
    EXPECT_EQ(clock.on_hand_started().big_blind, 30);
    EXPECT_TRUE(clock.level_changed());
    EXPECT_EQ(clock.level_index(), 1);
    EXPECT_EQ(clock.next_level()->big_blind, 40);
    clock.on_hand_started();
    EXPECT_FALSE(clock.level_changed());
    EXPECT_EQ(clock.on_hand_started().big_blind, 40);
    EXPECT_FALSE(clock.next_level());
    for (int i = 0; i < 10; ++i) EXPECT_EQ(clock.on_hand_started().big_blind, 40);  // The last level stays
    EXPECT_EQ(clock.hands_until_next_level(), 0);
}

TEST(TournamentClock, LevelsByMinutesAtTheNextHand) {
    TournamentClock clock(three_levels(), LevelUnit::Minutes, 1);
    EXPECT_EQ(clock.on_hand_started().big_blind, 20);
    for (int i = 0; i < 59; ++i) clock.advance_ms(1000.0);
    EXPECT_NEAR(clock.ms_until_next_level(), 1000.0, 1e-6);
    EXPECT_EQ(clock.on_hand_started().big_blind, 20);
    clock.advance_ms(1000.0);
    EXPECT_LE(clock.ms_until_next_level(), 0.0);   // Due, but the hand in play keeps its blinds
    EXPECT_EQ(clock.current_level().big_blind, 20);
    EXPECT_EQ(clock.on_hand_started().big_blind, 30);
    EXPECT_TRUE(clock.level_changed());
}

TEST(TournamentClock, AStalledFrameCountsAtMostOneSecondAndPauseStopsTheClock) {
    TournamentClock clock(three_levels(), LevelUnit::Minutes, 1);
    clock.advance_ms(30'000.0);
    EXPECT_DOUBLE_EQ(clock.elapsed_ms(), 1000.0);
    clock.set_paused(true);
    clock.advance_ms(500.0);
    EXPECT_DOUBLE_EQ(clock.elapsed_ms(), 1000.0);
    clock.set_paused(false);
    clock.advance_ms(-5.0);
    EXPECT_DOUBLE_EQ(clock.elapsed_ms(), 1000.0);
}

TEST(TournamentClock, ALongBreakCanSkipLevels) {
    TournamentClock clock(three_levels(), LevelUnit::Minutes, 1);
    clock.on_hand_started();
    for (int i = 0; i < 150; ++i) clock.advance_ms(1000.0);
    EXPECT_EQ(clock.on_hand_started().big_blind, 40);
    EXPECT_EQ(clock.level_index(), 2);
}
