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
