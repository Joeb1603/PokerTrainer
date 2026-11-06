#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "engine/game_engine.h"
#include "equity/equity.h"
#include "range/combos.h"
#include "range/hand_vs_range.h"
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

// ---- Hero against each combo, and against ranges ----------------------------

std::vector<Card> cards_of(std::initializer_list<const char*> texts) {
    std::vector<Card> cards;
    for (const char* text : texts) cards.push_back(card(text));
    return cards;
}

HoleCards hole(const char* a, const char* b) { return {card(a), card(b)}; }

std::bitset<kComboCount> all_combos() { return std::bitset<kComboCount>().set(); }

RangeEquityOptions range_options(int rivers_per_turn, std::uint64_t seed = 0, int threads = 0) {
    RangeEquityOptions options;
    options.flop_rivers_per_turn = rivers_per_turn;
    options.seed = seed;
    options.threads = threads;
    return options;
}

// The dealable combos, in index order.
std::vector<int> valid_combos(const ComboEquities& equities) {
    std::vector<int> combos;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (equities.valid.test(static_cast<std::size_t>(combo))) combos.push_back(combo);
    }
    return combos;
}

EquityOptions mc(int rollouts, std::uint64_t seed) {
    EquityOptions options;
    options.rollouts = rollouts;
    options.seed = seed;
    return options;
}

TEST(HandVsRange, RiverMatchesBruteForce) {
    const HoleCards hero = hole("Ah", "3d");
    const std::vector<Card> board = cards_of({"Kc", "Kd", "7h", "7s", "2c"});
    const ComboEquities equities = hero_vs_each_combo(hero, board, all_combos());
    const std::vector<int> combos = valid_combos(equities);
    ASSERT_EQ(combos.size(), 990u);  // C(45, 2)

    int checked = 0;
    std::array<int, 3> outcomes{};  // Losses, ties, wins
    for (std::size_t i = 0; i < combos.size(); i += 20) {
        const std::array<HoleCards, 1> villain{combo_cards(combos[i])};
        // On a full board every rollout is the same deal, so one is exact.
        const double expected = equity_vs_hands(hero, board, villain, mc(1, 0)).equity;
        EXPECT_EQ(equities.equity[static_cast<std::size_t>(combos[i])], static_cast<float>(expected))
            << combo_cards(combos[i])[0].to_string() << combo_cards(combos[i])[1].to_string();
        ++outcomes[static_cast<std::size_t>(expected * 2.0)];
        ++checked;
    }
    EXPECT_EQ(checked, 50);
    for (const int count : outcomes) EXPECT_GT(count, 0);  // Wins, ties and losses all checked
}

TEST(HandVsRange, TurnExactMatchesHighRolloutMonteCarlo) {
    const HoleCards hero = hole("8c", "7c");
    const std::vector<Card> board = cards_of({"Ah", "9s", "4c", "Td"});
    const ComboEquities equities = hero_vs_each_combo(hero, board, all_combos());
    const std::vector<int> combos = valid_combos(equities);
    ASSERT_EQ(combos.size(), 1035u);  // C(46, 2)

    std::mt19937_64 rng(5);
    for (int k = 0; k < 20; ++k) {
        const int combo = combos[static_cast<std::size_t>(rng() % combos.size())];
        const std::array<HoleCards, 1> villain{combo_cards(combo)};
        const double got = equities.equity[static_cast<std::size_t>(combo)];

        // Exact, river by river.
        double sum = 0.0;
        int rivers = 0;
        for (int id = 0; id < kDeckSize; ++id) {
            const Card river(id);
            const CardMask used = card_mask(board) | card_mask(hero) | combo_mask(combo);
            if ((used & card_mask(river)) != 0) continue;
            std::vector<Card> full = board;
            full.push_back(river);
            sum += equity_vs_hands(hero, full, villain, mc(1, 0)).equity;
            ++rivers;
        }
        EXPECT_EQ(rivers, 44);
        EXPECT_NEAR(got, sum / rivers, 1e-6);

        const EquityResult sampled = equity_vs_hands(hero, board, villain, mc(50'000, 100 + static_cast<std::uint64_t>(k)));
        EXPECT_NEAR(got, sampled.equity, 4.0 * sampled.std_error + 1e-6);
    }
}

TEST(HandVsRange, UniformRangeReproducesRandomEquity) {
    const WeightedRange uniform = WeightedRange::uniform();

    // Preflop, through the class matrix. It ignores which suits hero's own cards
    // block, a bias measured at 0.02 pp here, on top of the table's sampling error.
    {
        const HoleCards hero = hole("Ah", "Kd");
        const double got = *equity_vs_range(hero_vs_each_combo(hero, {}, all_combos()), uniform);
        const EquityResult random = equity_vs_random(hero, {}, 1, mc(200'000, 3));
        EXPECT_NEAR(got, random.equity, 4.0 * random.std_error + 0.002);
    }

    // `reference`: equity_vs_random with 2,000,000 rollouts (standard error 0.03 pp).
    struct Spot {
        HoleCards hero;
        std::vector<Card> board;
        double reference;
    };
    const std::array<Spot, 2> spots{Spot{hole("8c", "8d"), cards_of({"Ah", "9s", "4c"}), 0.6395},
                                    Spot{hole("Qh", "Jh"), cards_of({"Qd", "9s", "8h"}), 0.8279}};
    for (const Spot& spot : spots) {
        const EquityResult random = equity_vs_random(spot.hero, spot.board, 1, mc(200'000, 11));
        const double exact = *equity_vs_range(hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(0)), uniform);
        const double sampled = *equity_vs_range(hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(8)), uniform);
        EXPECT_NEAR(exact, random.equity, 4.0 * random.std_error);
        EXPECT_NEAR(exact, spot.reference, 0.0015);
        // The sample's own error on top of the Monte Carlo's: 0.4 pp rms and 1.25 pp
        // at worst in HandVsRangeBench.DISABLED_FlopSampleError, 0.3 pp for this seed.
        EXPECT_NEAR(sampled, random.equity, 4.0 * random.std_error + 0.004);
    }
}

// Exact flop enumeration, checked against a walk over every runout for a few
// combos, then the default K = 8 sample (seed 0) against it. At this seed the
// aggregate errors are 0.02-0.42 pp and the worst combo 2.0-2.9 pp; over 20
// seeds and six flops they reach 1.25 pp and 5.9 pp (DISABLED_FlopSampleError).
TEST(HandVsRange, FlopSampleCloseToExact) {
    struct Spot {
        HoleCards hero;
        std::vector<Card> board;
    };
    const std::array<Spot, 5> spots{Spot{hole("8c", "8d"), cards_of({"Ah", "9s", "4c"})},
                                    Spot{hole("Qh", "Jh"), cards_of({"Qd", "9s", "8h"})},
                                    Spot{hole("Ad", "5d"), cards_of({"Kd", "7d", "2c"})},
                                    Spot{hole("Kh", "Qs"), cards_of({"8h", "8d", "3s"})},
                                    Spot{hole("6s", "5s"), cards_of({"Tc", "9h", "7c"})}};
    const WeightedRange uniform = WeightedRange::uniform();
    for (const Spot& spot : spots) {
        const ComboEquities exact = hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(0));
        const ComboEquities sampled = hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(8));
        const std::vector<int> combos = valid_combos(exact);
        ASSERT_EQ(combos.size(), 1081u);  // C(47, 2)
        EXPECT_EQ(exact.valid, sampled.valid);

        // Independent oracle for the exact mode: every unordered runout.
        for (std::size_t i = 0; i < combos.size(); i += 271) {
            const HoleCards villain = combo_cards(combos[i]);
            const CardMask used = card_mask(spot.board) | card_mask(spot.hero) | combo_mask(combos[i]);
            std::array<Card, 5> full{};
            std::copy(spot.board.begin(), spot.board.end(), full.begin());
            int doubled = 0;
            int runouts = 0;
            for (int t = 0; t < kDeckSize; ++t) {
                if ((used & (CardMask{1} << t)) != 0) continue;
                for (int r = t + 1; r < kDeckSize; ++r) {
                    if ((used & (CardMask{1} << r)) != 0) continue;
                    full[3] = Card(t);
                    full[4] = Card(r);
                    const HandRank a = evaluate7(spot.hero, full);
                    const HandRank b = evaluate7(villain, full);
                    doubled += a < b ? 2 : a == b ? 1 : 0;
                    ++runouts;
                }
            }
            EXPECT_EQ(runouts, 990);
            EXPECT_NEAR(exact.equity[static_cast<std::size_t>(combos[i])], doubled / (2.0 * runouts), 1e-6);
        }

        double worst = 0.0;
        for (const int combo : combos) {
            worst = std::max(worst, std::abs(static_cast<double>(sampled.equity[static_cast<std::size_t>(combo)]) -
                                             exact.equity[static_cast<std::size_t>(combo)]));
        }
        EXPECT_NEAR(*equity_vs_range(sampled, uniform), *equity_vs_range(exact, uniform), 0.005)
            << spot.board[0].to_string();
        EXPECT_LE(worst, 0.06) << spot.board[0].to_string();
    }
}

TEST(HandVsRange, DeterministicAcrossThreadCounts) {
    const HoleCards hero = hole("Qh", "Jh");
    for (const std::vector<Card>& board : {cards_of({"Qd", "9s", "8h"}), cards_of({"Qd", "9s", "8h", "2c"})}) {
        const ComboEquities one = hero_vs_each_combo(hero, board, all_combos(), range_options(8, 7, 1));
        const ComboEquities three = hero_vs_each_combo(hero, board, all_combos(), range_options(8, 7, 3));
        const ComboEquities automatic = hero_vs_each_combo(hero, board, all_combos(), range_options(8, 7, 0));
        EXPECT_EQ(one.valid, three.valid);
        EXPECT_EQ(std::memcmp(one.equity.data(), three.equity.data(), sizeof(one.equity)), 0);
        EXPECT_EQ(std::memcmp(one.equity.data(), automatic.equity.data(), sizeof(one.equity)), 0);
    }

    // A different seed takes a different flop sample.
    const std::vector<Card> flop = cards_of({"Qd", "9s", "8h"});
    const ComboEquities a = hero_vs_each_combo(hero, flop, all_combos(), range_options(8, 7));
    const ComboEquities b = hero_vs_each_combo(hero, flop, all_combos(), range_options(8, 8));
    EXPECT_NE(std::memcmp(a.equity.data(), b.equity.data(), sizeof(a.equity)), 0);

    std::array<std::optional<WeightedRange>, kMaxSeats> ranges{};
    ranges[1] = WeightedRange::from_hand_range(HandRange::parse("22+, A2s+, KTs+, ATo+, KJo+"));
    ranges[4] = WeightedRange::uniform();
    const std::array<LayerRequest, 2> layers{LayerRequest{0b10010}, LayerRequest{0b00010}};
    RangeEquityOptions options = range_options(8, 21, 1);
    options.multiway_rollouts = 5000;  // Five chunks
    const MultiwayRangeEquity single = equity_vs_ranges(hero, flop, ranges, layers, options);
    options.threads = 3;
    const MultiwayRangeEquity threaded = equity_vs_ranges(hero, flop, ranges, layers, options);
    EXPECT_EQ(single.equity, threaded.equity);
    EXPECT_EQ(single.std_error, threaded.std_error);
    EXPECT_EQ(single.restarts, threaded.restarts);
    EXPECT_GT(single.restarts, 0u);
}

TEST(HandVsRange, NeededMaskLimitsWork) {
    const HoleCards hero = hole("Ah", "Kh");
    std::bitset<kComboCount> needed;
    const int aces = combo_index(card("As"), card("Ad"));
    const int blocked = combo_index(card("Ah"), card("Qc"));  // Holds hero's ace
    const int on_board = combo_index(card("Th"), card("2c"));  // Holds a board card
    needed.set(static_cast<std::size_t>(aces));
    needed.set(static_cast<std::size_t>(blocked));
    needed.set(static_cast<std::size_t>(on_board));

    for (const std::vector<Card>& board :
         {std::vector<Card>{}, cards_of({"Th", "7h", "2s"}), cards_of({"Th", "7h", "2s", "3d"}),
          cards_of({"Th", "7h", "2s", "3d", "9c"})}) {
        const ComboEquities equities = hero_vs_each_combo(hero, board, needed);
        EXPECT_TRUE(equities.valid.test(static_cast<std::size_t>(aces)));
        EXPECT_LT(equities.equity[static_cast<std::size_t>(aces)], 0.5f);  // Aces ahead of hero's AK
        EXPECT_FALSE(equities.valid.test(static_cast<std::size_t>(blocked)));
        EXPECT_EQ(equities.valid.test(static_cast<std::size_t>(on_board)), board.empty());
        EXPECT_EQ(equities.valid.count(), board.empty() ? 2u : 1u);
        for (int combo = 0; combo < kComboCount; ++combo) {
            if (!equities.valid.test(static_cast<std::size_t>(combo))) {
                EXPECT_EQ(equities.equity[static_cast<std::size_t>(combo)], 0.0f);
            }
        }

        // A range may hold dead combos, which are skipped, but not live ones
        // that were left out.
        WeightedRange range;
        range.set(aces, 1.0f);
        range.set(blocked, 1.0f);
        EXPECT_DOUBLE_EQ(*equity_vs_range(equities, range), equities.equity[static_cast<std::size_t>(aces)]);
        range.set(combo_index(card("Qs"), card("Qd")), 0.5f);
        EXPECT_THROW((void)equity_vs_range(equities, range), std::invalid_argument);
        WeightedRange dead_only;
        dead_only.set(blocked, 1.0f);
        EXPECT_FALSE(equity_vs_range(equities, dead_only).has_value());
        EXPECT_FALSE(equity_vs_range(equities, WeightedRange()).has_value());
    }

    EXPECT_EQ(combos_in(WeightedRange::from_hand_range(HandRange::parse("AA, AKs"))).count(), 10u);
    EXPECT_EQ(combos_in(WeightedRange()).count(), 0u);
}

TEST(HandVsRange, RejectsBadInput) {
    const HoleCards hero = hole("Ah", "Kh");
    EXPECT_THROW(hero_vs_each_combo(hero, cards_of({"Th", "7h"}), all_combos()), std::invalid_argument);
    EXPECT_THROW(hero_vs_each_combo(hero, cards_of({"Th", "7h", "Ah"}), all_combos()), std::invalid_argument);
    EXPECT_THROW(hero_vs_each_combo(hole("Ah", "Ah"), {}, all_combos()), std::invalid_argument);
    EXPECT_THROW(hero_vs_each_combo(hero, {}, all_combos(), range_options(-1)), std::invalid_argument);
    EXPECT_THROW(hero_vs_each_combo(hero, {}, all_combos(), range_options(8, 0, -1)), std::invalid_argument);

    const std::vector<Card> flop = cards_of({"Th", "7h", "2s"});
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges{};
    ranges[2] = WeightedRange::uniform();
    const std::array<LayerRequest, 1> good{LayerRequest{0b100}};
    EXPECT_NO_THROW(equity_vs_ranges(hero, flop, ranges, good));
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, {}), std::invalid_argument);
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, std::array{LayerRequest{0}}), std::invalid_argument);
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, std::array{LayerRequest{0b1000}}), std::invalid_argument);
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, std::array{LayerRequest{0b1000000}}), std::invalid_argument);
    EXPECT_THROW(equity_vs_ranges(hero, cards_of({"Th", "Ah", "2s"}), ranges, good), std::invalid_argument);
    RangeEquityOptions no_rollouts;
    no_rollouts.multiway_rollouts = 0;
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, good, no_rollouts), std::invalid_argument);

    // A range at every seat index leaves none for hero.
    {
        std::array<std::optional<WeightedRange>, kMaxSeats> six{};
        six.fill(WeightedRange::uniform());
        EXPECT_THROW(equity_vs_ranges(hero, flop, six, good), std::invalid_argument);
    }

    // Only combos hero or the board block: nothing left to deal.
    ranges[2] = WeightedRange();
    ranges[2]->set(combo_index(card("Ah"), card("Ad")), 1.0f);
    ranges[2]->set(combo_index(card("Th"), card("Td")), 1.0f);
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, good), std::invalid_argument);

    // Two villains who would both need the ace of spades.
    ranges[2] = WeightedRange();
    ranges[2]->set(combo_index(card("As"), card("Ad")), 1.0f);
    ranges[3] = WeightedRange();
    ranges[3]->set(combo_index(card("As"), card("Ac")), 1.0f);
    EXPECT_THROW(equity_vs_ranges(hero, flop, ranges, std::array{LayerRequest{0b1100}}), std::invalid_argument);
}

TEST(MultiwayRangeEquity, MatchesRandomWhenRangesUniform) {
    const HoleCards hero = hole("Qh", "Jh");
    const std::vector<Card> board = cards_of({"Qd", "9s", "8h"});
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges{};
    ranges[0] = WeightedRange::uniform();
    ranges[5] = WeightedRange::uniform();
    const std::array<LayerRequest, 2> layers{LayerRequest{0b100001}, LayerRequest{0b000001}};
    RangeEquityOptions options;
    options.seed = 4;
    options.multiway_rollouts = 40'000;
    const MultiwayRangeEquity got = equity_vs_ranges(hero, board, ranges, layers, options);
    ASSERT_EQ(got.equity.size(), 2u);
    EXPECT_EQ(got.rollouts, 40'000);

    // The second villain's cards still leave the deck in the heads-up layer, which
    // is the same distribution as a random hand.
    const EquityResult three_way = equity_vs_random(hero, board, 2, mc(40'000, 9));
    const EquityResult heads_up = equity_vs_random(hero, board, 1, mc(40'000, 10));
    EXPECT_NEAR(got.equity[0], three_way.equity, 4.0 * std::hypot(got.std_error[0], three_way.std_error));
    EXPECT_NEAR(got.equity[1], heads_up.equity, 4.0 * std::hypot(got.std_error[1], heads_up.std_error));
    EXPECT_LT(got.equity[0], got.equity[1]);
}

TEST(MultiwayRangeEquity, LayersSeeOnlyTheirOpponents) {
    const HoleCards hero = hole("8c", "8d");
    const std::vector<Card> board = cards_of({"Ah", "9s", "4c"});
    const WeightedRange strong = WeightedRange::from_hand_range(HandRange::parse("AA, 99, 44, AKs, AKo, AQs, AQo"));
    const WeightedRange weak = WeightedRange::from_hand_range(HandRange::parse("22-77, KQs, KQo, JTs, T8s, 65s"));

    std::array<std::optional<WeightedRange>, kMaxSeats> ranges{};
    ranges[1] = strong;
    ranges[3] = weak;
    const std::array<LayerRequest, 3> layers{LayerRequest{0b1010}, LayerRequest{0b0010}, LayerRequest{0b1000}};
    RangeEquityOptions options;
    options.seed = 12;
    options.multiway_rollouts = 20'000;
    const MultiwayRangeEquity both = equity_vs_ranges(hero, board, ranges, layers, options);
    EXPECT_LT(both.equity[1], 0.25);  // Against sets and aces
    EXPECT_GT(both.equity[2], 0.75);  // Against underpairs and draws
    EXPECT_LE(both.equity[0], std::min(both.equity[1], both.equity[2]));

    // A villain alone matches the exact heads-up dot product.
    const ComboEquities exact = hero_vs_each_combo(hero, board, all_combos(), range_options(0));
    for (const int seat_index : {1, 3}) {
        std::array<std::optional<WeightedRange>, kMaxSeats> alone{};
        alone[static_cast<std::size_t>(seat_index)] = ranges[static_cast<std::size_t>(seat_index)];
        const std::array<LayerRequest, 1> layer{LayerRequest{static_cast<std::uint8_t>(1u << seat_index)}};
        const MultiwayRangeEquity got = equity_vs_ranges(hero, board, alone, layer, options);
        const double expected = *equity_vs_range(exact, *alone[static_cast<std::size_t>(seat_index)]);
        EXPECT_NEAR(got.equity[0], expected, 4.0 * got.std_error[0]) << seat_index;
        EXPECT_EQ(got.restarts, 0u);
    }
}

// Timing report, not run by default:
// core_tests --gtest_also_run_disabled_tests --gtest_filter='HandVsRangeBench*'
template <typename Call>
void report_timing(const char* label, int runs, const Call& call) {
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        call(i);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(ms.begin(), ms.end());
    const auto at = [&](double q) { return ms[static_cast<std::size_t>(q * static_cast<double>(ms.size() - 1))]; };
    std::printf("%-34s p50 %8.3f ms  p95 %8.3f ms  max %8.3f ms\n", label, at(0.5), at(0.95), ms.back());
}

TEST(HandVsRangeBench, DISABLED_Timing) {
    constexpr int kRuns = 60;
    const HoleCards hero = hole("8c", "8d");
    const std::vector<Card> river = cards_of({"Ah", "9s", "4c", "Kd", "2h"});
    const std::span<const Card> turn(river.data(), 4);
    const std::span<const Card> flop(river.data(), 3);
    float sink = 0.0f;
    for (const int threads : {1, 0, 4}) {
        const char* suffix = threads == 1 ? "1 thread" : threads == 0 ? "auto" : "4 threads";
        const auto run = [&](std::span<const Card> board, int rivers_per_turn, const char* street) {
            const std::string label = std::string(street) + ", " + suffix;
            report_timing(label.c_str(), kRuns, [&](int i) {
                const ComboEquities e = hero_vs_each_combo(
                    hero, board, all_combos(), range_options(rivers_per_turn, static_cast<std::uint64_t>(i), threads));
                sink += e.equity[100];
            });
        };
        run(river, 8, "river");
        run(turn, 8, "turn");
        run(flop, 8, "flop K=8");
        run(flop, 12, "flop K=12");
        run(flop, 0, "flop exact");
        run({}, 8, "preflop");

        std::array<std::optional<WeightedRange>, kMaxSeats> ranges{};
        ranges[3] = WeightedRange::from_hand_range(rfi_range(Position::BTN));
        ranges[5] = WeightedRange::from_hand_range(HandRange::parse("22+, A2s+, K5s+, Q8s+, J8s+, T8s+, 97s+, 86s+, 75s+, 65s, ATo+, KTo+, QTo+, JTo"));
        const std::array<LayerRequest, 2> layers{LayerRequest{0b101000}, LayerRequest{0b001000}};
        for (const std::span<const Card> board : std::array<std::span<const Card>, 3>{flop, turn, river}) {
            const std::string label = "multiway 3-way " + std::to_string(board.size()) + " cards, " + suffix;
            report_timing(label.c_str(), kRuns, [&](int i) {
                RangeEquityOptions options = range_options(8, static_cast<std::uint64_t>(i), threads);
                sink += static_cast<float>(equity_vs_ranges(hero, board, ranges, layers, options).equity[0]);
            });
        }
    }
    EXPECT_GT(sink, 0.0f);
}

// How far the flop sample lands from exact enumeration, over several flops and
// seeds: the aggregate against a uniform range, and the worst single combo.
TEST(HandVsRangeBench, DISABLED_FlopSampleError) {
    struct Spot {
        HoleCards hero;
        std::vector<Card> board;
    };
    const std::array<Spot, 6> spots{Spot{hole("8c", "8d"), cards_of({"Ah", "9s", "4c"})},
                                    Spot{hole("Qh", "Jh"), cards_of({"Qd", "9s", "8h"})},
                                    Spot{hole("Ad", "5d"), cards_of({"Kd", "7d", "2c"})},
                                    Spot{hole("Kh", "Qs"), cards_of({"8h", "8d", "3s"})},
                                    Spot{hole("6s", "5s"), cards_of({"Tc", "9h", "7c"})},
                                    Spot{hole("Ac", "Kc"), cards_of({"Jd", "6s", "2h"})}};
    const WeightedRange uniform = WeightedRange::uniform();
    const WeightedRange defend = WeightedRange::from_hand_range(HandRange::parse("22+, A2s+, K5s+, Q8s+, J8s+, T8s+, 97s+, 86s+, 75s+, 65s, ATo+, KTo+, QTo+, JTo"));
    for (const int k : {4, 8, 12, 16}) {
        double worst_aggregate = 0.0;
        double worst_defend = 0.0;
        double worst_combo = 0.0;
        double squared = 0.0;
        double squared_combo = 0.0;
        int samples = 0;
        int combo_samples = 0;
        for (const Spot& spot : spots) {
            const ComboEquities exact = hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(0));
            const double truth = *equity_vs_range(exact, uniform);
            const double truth_defend = *equity_vs_range(exact, defend);
            for (std::uint64_t seed = 0; seed < 20; ++seed) {
                const ComboEquities got = hero_vs_each_combo(spot.hero, spot.board, all_combos(), range_options(k, seed));
                const double error = *equity_vs_range(got, uniform) - truth;
                worst_aggregate = std::max(worst_aggregate, std::abs(error));
                worst_defend = std::max(worst_defend, std::abs(*equity_vs_range(got, defend) - truth_defend));
                squared += error * error;
                ++samples;
                for (int combo = 0; combo < kComboCount; ++combo) {
                    if (!exact.valid.test(static_cast<std::size_t>(combo))) continue;
                    const double d = static_cast<double>(got.equity[static_cast<std::size_t>(combo)]) -
                                     exact.equity[static_cast<std::size_t>(combo)];
                    worst_combo = std::max(worst_combo, std::abs(d));
                    squared_combo += d * d;
                    ++combo_samples;
                }
            }
        }
        std::printf("K=%2d  uniform: rms %.3f pp, worst %.3f pp | defend range worst %.3f pp | combo: rms %.2f pp, worst %.2f pp\n",
                    k, 100.0 * std::sqrt(squared / samples), 100.0 * worst_aggregate, 100.0 * worst_defend,
                    100.0 * std::sqrt(squared_combo / combo_samples), 100.0 * worst_combo);
    }
}

}  // namespace
