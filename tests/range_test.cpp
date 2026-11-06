#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "engine/game_engine.h"
#include "equity/equity.h"
#include "range/combos.h"
#include "range/preflop_matrix.h"
#include "range/weighted_range.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

HandClass hand(const char* text) { return HandClass::parse(text).value(); }

TEST(Combos, IndexRoundTripsAndCovers1326) {
    std::set<int> seen;
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            const int combo = combo_index(Card(a), Card(b));
            ASSERT_EQ(combo, combo_index(Card(b), Card(a)));
            ASSERT_GE(combo, 0);
            ASSERT_LT(combo, kComboCount);
            seen.insert(combo);

            const std::array<Card, 2> cards = combo_cards(combo);
            EXPECT_EQ(cards[0], Card(a));
            EXPECT_EQ(cards[1], Card(b));
            EXPECT_EQ(combo_mask(combo), (CardMask{1} << a) | (CardMask{1} << b));
            EXPECT_EQ(combo_class(combo), HandClass::from_cards(Card(a), Card(b)));
        }
    }
    EXPECT_EQ(static_cast<int>(seen.size()), kComboCount);
    EXPECT_EQ(card_mask(std::array{card("As"), card("2c")}), (CardMask{1} << 51) | CardMask{1});
}

TEST(Combos, ClassCountsAre6_4_12) {
    EXPECT_EQ(class_combos(hand("AA")).count, 6);
    EXPECT_EQ(class_combos(hand("AKs")).count, 4);
    EXPECT_EQ(class_combos(hand("AKo")).count, 12);

    int total = 0;
    std::set<int> seen;
    for (const HandClass h : hand_classes_by_strength()) {
        const ClassCombos& list = class_combos(h);
        EXPECT_EQ(list.count, h.combos()) << h.to_string();
        EXPECT_TRUE(std::is_sorted(list.view().begin(), list.view().end()));
        for (const std::uint16_t combo : list.view()) {
            EXPECT_EQ(combo_class(combo), h);
            seen.insert(combo);
        }
        total += list.count;
    }
    EXPECT_EQ(total, kComboCount);
    EXPECT_EQ(static_cast<int>(seen.size()), kComboCount);
}

TEST(WeightedRange, FromHandRangeMatchesComboCount) {
    const HandRange& utg = rfi_range(Position::UTG);
    EXPECT_DOUBLE_EQ(WeightedRange::from_hand_range(utg).total(), utg.combos());
    EXPECT_DOUBLE_EQ(WeightedRange::from_hand_range(utg, 0.5f).total(), 0.5 * utg.combos());
    EXPECT_DOUBLE_EQ(WeightedRange().total(), 0.0);

    WeightedRange all = WeightedRange::uniform();
    EXPECT_DOUBLE_EQ(all.total(), kComboCount);
    all.remove_cards(card_mask(card("As")) | card_mask(card("Kd")));
    EXPECT_DOUBLE_EQ(all.total(), 1225.0);  // C(50, 2)
    EXPECT_DOUBLE_EQ(all.class_mass(hand("AA")), 3.0);
    EXPECT_DOUBLE_EQ(all.class_mass(hand("AKs")), 2.0);
    EXPECT_DOUBLE_EQ(all.class_mass(hand("AKo")), 7.0);  // 12 less the 5 holding As or Kd
    EXPECT_EQ(all.weight(combo_index(card("As"), card("Qh"))), 0.0f);
    EXPECT_EQ(all.weight(combo_index(card("Ah"), card("Qh"))), 1.0f);
}

TEST(WeightedRange, ShareOfAndMultiply) {
    WeightedRange range = WeightedRange::from_hand_range(HandRange::parse("AA, KK"));
    const WeightedRange aces = WeightedRange::from_hand_range(HandRange::parse("AA"));
    EXPECT_DOUBLE_EQ(range.share_of(aces), 0.5);
    EXPECT_DOUBLE_EQ(range.share_of(WeightedRange::uniform()), 1.0);
    EXPECT_DOUBLE_EQ(range.share_of(WeightedRange()), 0.0);
    EXPECT_DOUBLE_EQ(WeightedRange().share_of(aces), 0.0);  // Empty range: no share, not NaN

    // Membership is clamped to [0, 1], so weights above 1 cannot inflate a share.
    WeightedRange heavy = WeightedRange::from_hand_range(HandRange::parse("AA"), 3.0f);
    EXPECT_DOUBLE_EQ(range.share_of(heavy), 0.5);

    // Half of the kings: their mass halves, and aces become two thirds of the range.
    WeightedRange filter = WeightedRange::uniform();
    filter.set_class(hand("KK"), 0.5f);
    range.multiply(filter);
    EXPECT_DOUBLE_EQ(range.total(), 9.0);
    EXPECT_DOUBLE_EQ(range.class_mass(hand("KK")), 3.0);
    EXPECT_DOUBLE_EQ(range.share_of(aces), 6.0 / 9.0);

    range.multiply(WeightedRange());
    EXPECT_DOUBLE_EQ(range.total(), 0.0);
}

// Same spot as SidePots in bots_test.cpp: a 40-chip big blind all in preflop, then
// 1000 bet on the river by seat 1 into seat 0. Button on seat 0, seat 2 short.
GameEngine side_pot_river() {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 40, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = std::array{card("Qc"), card("5d")};
    deal.holes[1] = std::array{card("Jc"), card("Th")};
    deal.holes[2] = std::array{card("8c"), card("6h")};
    deal.board = {card("Ks"), card("7c"), card("2d"), card("9h"), card("4s")};
    engine.start_hand(deal);
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

TEST(PotLayers, SeatMaskMatchesCounts) {
    const GameEngine engine = side_pot_river();
    const std::vector<PotLayer> layers = pot_layers(engine.player_view(0));
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].opponent_seats, 0b110);  // Main pot: the bettor and the short all-in
    EXPECT_EQ(layers[1].opponent_seats, 0b010);  // Side pot: the bettor alone
    for (const PotLayer& layer : layers) {
        EXPECT_EQ(std::popcount(layer.opponent_seats), layer.opponents);
        EXPECT_EQ(layer.opponent_seats & 0b001, 0);  // Never the viewer
    }
}

// Upper bound on a table entry's standard error: a share in {0, 0.5, 1} has
// variance at most 0.25.
double matrix_std_error() { return 0.5 / std::sqrt(static_cast<double>(preflop_matrix_rollouts())); }

TEST(PreflopMatrix, KnownAnswers) {
    ASSERT_GT(preflop_matrix_rollouts(), 0);
    // Published all-in equities, averaged over suits.
    EXPECT_NEAR(class_vs_class_equity(hand("AA"), hand("KK")), 0.819, 0.01);
    EXPECT_NEAR(class_vs_class_equity(hand("22"), hand("AKo")), 0.525, 0.01);
    EXPECT_NEAR(class_vs_class_equity(hand("QQ"), hand("AKo")), 0.565, 0.01);
    EXPECT_NEAR(class_vs_class_equity(hand("KK"), hand("AA")), 0.181, 0.01);

    std::array<HandClass, kHandClassCount> by_index{};
    for (const HandClass h : hand_classes_by_strength()) by_index[static_cast<std::size_t>(h.index())] = h;
    const double diagonal_tolerance = 4.5 * matrix_std_error() + 1.0 / 65535.0;
    for (const HandClass a : by_index) {
        EXPECT_NEAR(class_vs_class_equity(a, a), 0.5, diagonal_tolerance) << a.to_string();
        for (const HandClass b : by_index) {
            if (a == b) continue;
            ASSERT_NEAR(class_vs_class_equity(a, b) + class_vs_class_equity(b, a), 1.0, 2.0 / 65535.0)
                << a.to_string() << " vs " << b.to_string();
        }
    }
}

// Independent check: equity_vs_hands over every non-conflicting pair of combos,
// weighted equally, against the table's sampled combos.
TEST(PreflopMatrix, SpotChecksAgainstMonteCarlo) {
    constexpr int kPairs = 10;
    constexpr int kRolloutsPerClassPair = 300'000;
    const std::array<HandClass, kHandClassCount>& order = hand_classes_by_strength();
    std::mt19937_64 rng(77);
    for (int k = 0; k < kPairs; ++k) {
        const HandClass a = order[static_cast<std::size_t>(rng() % kHandClassCount)];
        const HandClass b = order[static_cast<std::size_t>(rng() % kHandClassCount)];

        std::vector<std::array<int, 2>> combo_pairs;
        for (const std::uint16_t x : class_combos(a).view()) {
            for (const std::uint16_t y : class_combos(b).view()) {
                if ((combo_mask(x) & combo_mask(y)) == 0) combo_pairs.push_back({x, y});
            }
        }
        ASSERT_FALSE(combo_pairs.empty());

        const int rollouts = std::max(1000, kRolloutsPerClassPair / static_cast<int>(combo_pairs.size()));
        double sum = 0.0;
        double variance_sum = 0.0;
        for (std::size_t i = 0; i < combo_pairs.size(); ++i) {
            const std::array<HoleCards, 1> villain{combo_cards(combo_pairs[i][1])};
            EquityOptions options;
            options.rollouts = rollouts;
            options.seed = 1000 * static_cast<std::uint64_t>(k) + i;
            const EquityResult result = equity_vs_hands(combo_cards(combo_pairs[i][0]), {}, villain, options);
            sum += result.equity;
            variance_sum += result.std_error * result.std_error;
        }
        const double n = static_cast<double>(combo_pairs.size());
        const double expected = sum / n;
        const double independent_error = std::sqrt(variance_sum) / n;
        const double tolerance =
            std::max(3.0 * std::hypot(independent_error, matrix_std_error()), 0.006);
        EXPECT_NEAR(class_vs_class_equity(a, b), expected, tolerance) << a.to_string() << " vs " << b.to_string();
    }
}

}  // namespace
