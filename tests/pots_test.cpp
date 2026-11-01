#include <gtest/gtest.h>

#include "engine/pots.h"

using namespace poker;

namespace {

constexpr std::array<bool, kMaxSeats> kAllLive{true, true, true, true, true, true};

}  // namespace

TEST(Pots, EqualContributionsMakeOnePot) {
    const auto result = calculate_pots({100, 100, 100, 0, 0, 0}, kAllLive);
    ASSERT_EQ(result.pots.size(), 1u);
    EXPECT_EQ(result.pots[0].amount, 300);
    EXPECT_EQ(result.pots[0].eligible, (std::vector<int>{0, 1, 2}));
    EXPECT_FALSE(result.refund);
}

TEST(Pots, ThreeWayAllInMakesMainAndSidePotPlusRefund) {
    const auto result = calculate_pots({100, 300, 500, 0, 0, 0}, kAllLive);
    ASSERT_EQ(result.pots.size(), 2u);
    EXPECT_EQ(result.pots[0].amount, 300);
    EXPECT_EQ(result.pots[0].eligible, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(result.pots[1].amount, 400);
    EXPECT_EQ(result.pots[1].eligible, (std::vector<int>{1, 2}));
    ASSERT_TRUE(result.refund);
    EXPECT_EQ(result.refund->seat, 2);
    EXPECT_EQ(result.refund->amount, 200);
}

TEST(Pots, SixDifferentStacksMakeFivePotsAndARefund) {
    const auto result = calculate_pots({10, 20, 30, 40, 50, 60}, kAllLive);
    ASSERT_EQ(result.pots.size(), 5u);
    EXPECT_EQ(result.pots[0].amount, 60);
    EXPECT_EQ(result.pots[1].amount, 50);
    EXPECT_EQ(result.pots[2].amount, 40);
    EXPECT_EQ(result.pots[3].amount, 30);
    EXPECT_EQ(result.pots[4].amount, 20);
    EXPECT_EQ(result.pots[4].eligible, (std::vector<int>{4, 5}));
    ASSERT_TRUE(result.refund);
    EXPECT_EQ(result.refund->seat, 5);
    EXPECT_EQ(result.refund->amount, 10);
}

TEST(Pots, FoldedChipsStayInButFolderIsNotEligible) {
    const auto result = calculate_pots({50, 100, 100, 0, 0, 0}, {false, true, true, true, true, true});
    ASSERT_EQ(result.pots.size(), 1u);
    EXPECT_EQ(result.pots[0].amount, 250);
    EXPECT_EQ(result.pots[0].eligible, (std::vector<int>{1, 2}));
    EXPECT_FALSE(result.refund);
}

TEST(Pots, WalkRefundsTheUncalledPartOfTheBigBlind) {
    const auto result = calculate_pots({0, 10, 20, 0, 0, 0}, {false, false, true, false, false, false});
    ASSERT_EQ(result.pots.size(), 1u);
    EXPECT_EQ(result.pots[0].amount, 20);
    EXPECT_EQ(result.pots[0].eligible, (std::vector<int>{2}));
    ASSERT_TRUE(result.refund);
    EXPECT_EQ(result.refund->seat, 2);
    EXPECT_EQ(result.refund->amount, 10);
}

TEST(Pots, FolderContributingMostMergesIntoThePotBelow) {
    // Seat 2 folded after putting in more than either player still in the hand.
    const auto result = calculate_pots({40, 40, 100, 0, 0, 0}, {true, true, false, false, false, false});
    ASSERT_EQ(result.pots.size(), 1u);
    EXPECT_EQ(result.pots[0].amount, 180);
    EXPECT_EQ(result.pots[0].eligible, (std::vector<int>{0, 1}));
    EXPECT_FALSE(result.refund);
}

TEST(Pots, LayersWithSameEligibleSetsMerge) {
    // Seat 0 folded at 30, so the 30 and 60 layers are contested by the same two.
    const auto result = calculate_pots({30, 60, 60, 0, 0, 0}, {false, true, true, true, true, true});
    ASSERT_EQ(result.pots.size(), 1u);
    EXPECT_EQ(result.pots[0].amount, 150);
}

TEST(Pots, ShortStackInSidePotScenarioConservesChips) {
    const std::array<int, kMaxSeats> contributed{25, 80, 80, 200, 200, 13};
    const std::array<bool, kMaxSeats> live{true, false, true, true, true, false};
    const auto result = calculate_pots(contributed, live);

    int total = result.refund ? result.refund->amount : 0;
    for (const Pot& pot : result.pots) total += pot.amount;
    EXPECT_EQ(total, 598);

    EXPECT_EQ(result.pots.front().eligible, (std::vector<int>{0, 2, 3, 4}));
    EXPECT_EQ(result.pots.back().eligible, (std::vector<int>{3, 4}));
}

// --- Live pots ---------------------------------------------------------------

namespace {
constexpr SeatStatus A = SeatStatus::Active;
constexpr SeatStatus I = SeatStatus::AllIn;
constexpr SeatStatus F = SeatStatus::Folded;
constexpr SeatStatus B = SeatStatus::Busted;
}  // namespace

TEST(LivePots, NoAllInsMeansOnePotEvenWithUnequalBets) {
    // 100, 200 and 300 with chips behind: calculate_pots would show three layers.
    const auto live = calculate_live_pots({100, 200, 300, 0, 0, 0}, {A, A, A, F, F, F});
    EXPECT_EQ(live.total, 600);
    ASSERT_EQ(live.pots.size(), 1u);
    EXPECT_EQ(live.pots[0].amount, 500);
    EXPECT_EQ(live.pots[0].eligible, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(live.uncalled, 100);
}

TEST(LivePots, BlindsShowTheUnmatchedPartOfTheBigBlind) {
    const auto live = calculate_live_pots({0, 10, 20, 0, 0, 0}, {A, A, A, A, A, A});
    EXPECT_EQ(live.total, 30);
    ASSERT_EQ(live.pots.size(), 1u);
    EXPECT_EQ(live.pots[0].amount, 20);
    EXPECT_EQ(live.uncalled, 10);
}

TEST(LivePots, MultiWayAllInSplitsMainAndSidePots) {
    const auto live = calculate_live_pots({100, 300, 300, 0, 0, 0}, {I, A, A, F, F, F});
    ASSERT_EQ(live.pots.size(), 2u);
    EXPECT_EQ(live.pots[0].amount, 300);
    EXPECT_EQ(live.pots[0].eligible, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(live.pots[1].amount, 400);
    EXPECT_EQ(live.pots[1].eligible, (std::vector<int>{1, 2}));
    EXPECT_EQ(live.uncalled, 0);
}

TEST(LivePots, TwoAllInLevelsMakeTwoSidePots) {
    const auto live = calculate_live_pots({50, 200, 500, 500, 0, 0}, {I, I, A, A, F, F});
    ASSERT_EQ(live.pots.size(), 3u);
    EXPECT_EQ(live.pots[0].amount, 200);
    EXPECT_EQ(live.pots[1].amount, 450);
    EXPECT_EQ(live.pots[1].eligible, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(live.pots[2].amount, 600);
    EXPECT_EQ(live.pots[2].eligible, (std::vector<int>{2, 3}));
}

TEST(LivePots, ShoveNobodyHasCalledCountsAsUncalled) {
    // Seat 0 shoves 500; the blinds have yet to respond, and can still call.
    const auto live = calculate_live_pots({500, 10, 20, 0, 0, 0}, {I, A, A, F, F, F});
    EXPECT_EQ(live.uncalled, 480);
    ASSERT_EQ(live.pots.size(), 1u);
    EXPECT_EQ(live.pots[0].amount, 50);
    EXPECT_EQ(live.pots[0].eligible, (std::vector<int>{0, 1, 2}));
}

TEST(LivePots, FoldedChipsStayInPlayAndAreNeverUncalled) {
    const auto live = calculate_live_pots({100, 40, 40, 0, 0, 0}, {F, A, A, B, B, B});
    EXPECT_EQ(live.uncalled, 0);
    ASSERT_EQ(live.pots.size(), 1u);
    EXPECT_EQ(live.pots[0].amount, 180);
    EXPECT_EQ(live.pots[0].eligible, (std::vector<int>{1, 2}));
}

TEST(LivePots, NothingBetIsEmpty) {
    const auto live = calculate_live_pots({}, {A, A, A, A, A, A});
    EXPECT_EQ(live.total, 0);
    EXPECT_TRUE(live.pots.empty());
    EXPECT_EQ(live.uncalled, 0);
}

// Once betting closes, the display must agree with what is actually paid.
TEST(LivePots, MatchesFinalPotsOnceAllBetsAreMade) {
    const std::array<int, kMaxSeats> contributed{100, 300, 500, 0, 0, 0};
    const auto live = calculate_live_pots(contributed, {I, I, A, F, F, F});
    const auto final_pots = calculate_pots(contributed, {true, true, true, false, false, false});
    ASSERT_EQ(live.pots.size(), final_pots.pots.size());
    for (std::size_t i = 0; i < live.pots.size(); ++i) {
        EXPECT_EQ(live.pots[i].amount, final_pots.pots[i].amount);
        EXPECT_EQ(live.pots[i].eligible, final_pots.pots[i].eligible);
    }
    EXPECT_EQ(live.uncalled, final_pots.refund->amount);
}
