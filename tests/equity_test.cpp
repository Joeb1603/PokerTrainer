#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/deck.h"
#include "core/hand_evaluator.h"
#include "equity/equity.h"

using namespace poker;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }
HoleCards hole(const char* a, const char* b) { return {card(a), card(b)}; }

std::vector<Card> board_of(std::initializer_list<const char*> cards) {
    std::vector<Card> board;
    for (const char* c : cards) board.push_back(card(c));
    return board;
}

// Monte Carlo tolerance: a 4-sigma band fails by chance about 1 time in 16,000.
constexpr double kSigmas = 4.0;

// Published figures are quoted to one decimal, so can be off by up to this much.
constexpr double kPublishedRounding = 0.0005;

constexpr int kConvergenceRollouts = 400'000;

EquityOptions opts(int rollouts, std::uint64_t seed, int threads = 0) {
    EquityOptions options;
    options.rollouts = rollouts;
    options.seed = seed;
    options.threads = threads;
    return options;
}

void expect_converges(const EquityResult& result, double expected, double slack) {
    EXPECT_NEAR(result.equity, expected, kSigmas * result.std_error + slack)
        << "se=" << result.std_error << " win=" << result.win << " tie=" << result.tie;
    // The tolerance only means something if the standard error is itself small, and
    // the largest possible, for outcomes in [0, 1], is 0.5 / sqrt(n).
    EXPECT_LE(result.std_error, 0.5 / std::sqrt(static_cast<double>(result.rollouts)));
}

// Exact heads-up equity by walking every possible runout. Independent of the code
// under test apart from the evaluator, so gives a reference with no sampling noise.
double exact_heads_up(const HoleCards& hero, const HoleCards& villain, std::vector<Card> board) {
    std::array<bool, kDeckSize> used{};
    for (const Card c : hero) used[c.id] = true;
    for (const Card c : villain) used[c.id] = true;
    for (const Card c : board) used[c.id] = true;

    std::vector<Card> rest;
    for (int id = 0; id < kDeckSize; ++id) {
        if (!used[static_cast<std::size_t>(id)]) rest.emplace_back(id);
    }

    std::array<Card, 5> full{};
    std::copy(board.begin(), board.end(), full.begin());
    const int fixed = static_cast<int>(board.size());

    double total = 0.0;
    long long count = 0;
    const auto walk = [&](auto&& self, int depth, std::size_t from) -> void {
        if (depth == 5) {
            const HandRank a = evaluate7(hero, full);
            const HandRank b = evaluate7(villain, full);
            total += a < b ? 1.0 : a == b ? 0.5 : 0.0;
            ++count;
            return;
        }
        for (std::size_t i = from; i < rest.size(); ++i) {
            full[static_cast<std::size_t>(depth)] = rest[i];
            self(self, depth + 1, i + 1);
        }
    };
    walk(walk, fixed, 0);
    return total / static_cast<double>(count);
}

}  // namespace

// --- Convergence on established values ---------------------------------------

TEST(EquityConvergence, AcesVersusOneRandomHand) {
    const auto result = equity_vs_random(hole("As", "Ad"), {}, 1, opts(kConvergenceRollouts, 101));
    expect_converges(result, 0.852, kPublishedRounding);
}

TEST(EquityConvergence, SevenTwoOffsuitVersusOneRandomHand) {
    const auto result = equity_vs_random(hole("7s", "2d"), {}, 1, opts(kConvergenceRollouts, 102));
    expect_converges(result, 0.346, kPublishedRounding);
}

TEST(EquityConvergence, AceKingSuitedVersusOneRandomHand) {
    const auto result = equity_vs_random(hole("As", "Ks"), {}, 1, opts(kConvergenceRollouts, 103));
    expect_converges(result, 0.670, kPublishedRounding);
}

TEST(EquityConvergence, AcesVersusKings) {
    const HoleCards aces = hole("As", "Ah");
    const HoleCards kings = hole("Kd", "Kc");
    const std::array<HoleCards, 1> villains{kings};
    const auto result = equity_vs_hands(aces, {}, villains, opts(kConvergenceRollouts, 104));

    // Exact value for these suits, which itself matches the commonly quoted 82%.
    const double exact = exact_heads_up(aces, kings, {});
    EXPECT_NEAR(exact, 0.82, 0.01);
    expect_converges(result, exact, 0.0);
}

TEST(EquityConvergence, AceKingOffsuitVersusQueens) {
    const HoleCards ace_king = hole("As", "Kd");
    const HoleCards queens = hole("Qh", "Qc");
    const std::array<HoleCards, 1> villains{queens};
    const auto result = equity_vs_hands(ace_king, {}, villains, opts(kConvergenceRollouts, 105));

    const double exact = exact_heads_up(ace_king, queens, {});
    EXPECT_NEAR(exact, 0.43, 0.01);
    expect_converges(result, exact, 0.0);
}

TEST(EquityConvergence, AcesVersusFiveRandomHands) {
    const auto result = equity_vs_random(hole("As", "Ad"), {}, 5, opts(kConvergenceRollouts, 106));
    // Quoted only as "about 49%", so allow for rounding to the nearest percent.
    expect_converges(result, 0.49, 0.005);
}

TEST(EquityConvergence, NutFlushDrawVersusSetOnTheFlop) {
    const HoleCards draw = hole("Ah", "Kh");
    const HoleCards set = hole("2s", "2d");
    const auto board = board_of({"2h", "7h", "Tc"});
    const std::array<HoleCards, 1> villains{set};
    const auto result = equity_vs_hands(draw, board, villains, opts(kConvergenceRollouts, 107));

    const double exact = exact_heads_up(draw, set, board);
    // 9 clean outs twice gives 1 - (36/45)(35/44) = 36.4%, an upper limit since
    // some of those runouts pair the board and give the set a full house.
    EXPECT_LT(exact, 0.364);
    EXPECT_GT(exact, 0.25);
    expect_converges(result, exact, 0.0);
}

TEST(EquityConvergence, TurnBoardMatchesExactEnumeration) {
    const HoleCards hero = hole("Ah", "Qh");
    const HoleCards villain = hole("Kc", "Kd");
    const auto board = board_of({"2h", "7h", "Tc", "3s"});
    const std::array<HoleCards, 1> villains{villain};
    const auto result = equity_vs_hands(hero, board, villains, opts(kConvergenceRollouts, 108));

    // Nine flush outs and three aces of 44 river cards; a queen pairs below the kings.
    EXPECT_DOUBLE_EQ(exact_heads_up(hero, villain, board), 12.0 / 44.0);
    expect_converges(result, 12.0 / 44.0, 0.0);
}

TEST(EquityConvergence, MultiwayKnownHandsShareEquityToOne) {
    // Same seed, same deck order, so each runout is shared and shares sum exactly.
    const HoleCards a = hole("As", "Ah");
    const HoleCards b = hole("Kd", "Kc");
    const HoleCards c = hole("9s", "8s");
    const auto options = opts(50'000, 109);

    const std::array<HoleCards, 2> vs_a{b, c};
    const std::array<HoleCards, 2> vs_b{a, c};
    const std::array<HoleCards, 2> vs_c{a, b};
    const double total = equity_vs_hands(a, {}, vs_a, options).equity + equity_vs_hands(b, {}, vs_b, options).equity +
                         equity_vs_hands(c, {}, vs_c, options).equity;
    EXPECT_NEAR(total, 1.0, 1e-9);
}

TEST(EquityConvergence, RandomOpponentOnTheRiverMatchesEveryPossibleHand) {
    // With the board complete, walking all 990 villain hands gives the exact value,
    // so this would catch any bias in how random opponents are dealt.
    const HoleCards hero = hole("Kh", "Qd");
    const auto board = board_of({"Ks", "7c", "2d", "9h", "4s"});

    std::array<Card, 5> full{};
    std::copy(board.begin(), board.end(), full.begin());
    std::array<bool, kDeckSize> used{};
    for (const Card c : hero) used[c.id] = true;
    for (const Card c : board) used[c.id] = true;

    const HandRank hero_rank = evaluate7(hero, full);
    double total = 0.0;
    int hands = 0;
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            if (used[static_cast<std::size_t>(a)] || used[static_cast<std::size_t>(b)]) continue;
            const HandRank villain = evaluate7(HoleCards{Card(a), Card(b)}, full);
            total += hero_rank < villain ? 1.0 : hero_rank == villain ? 0.5 : 0.0;
            ++hands;
        }
    }
    ASSERT_EQ(hands, 990);

    const auto result = equity_vs_random(hero, board, 1, opts(kConvergenceRollouts, 110));
    expect_converges(result, total / hands, 0.0);
}

// --- Exact results when no card is left to come ------------------------------

TEST(EquityExact, CertainWin) {
    const auto board = board_of({"Qs", "Js", "Ts", "2c", "3d"});
    const std::array<HoleCards, 1> villain{hole("Ah", "Ad")};
    const auto result = equity_vs_hands(hole("As", "Ks"), board, villain, opts(1000, 1));
    EXPECT_DOUBLE_EQ(result.equity, 1.0);
    EXPECT_DOUBLE_EQ(result.win, 1.0);
    EXPECT_DOUBLE_EQ(result.tie, 0.0);
    EXPECT_DOUBLE_EQ(result.std_error, 0.0);
    EXPECT_EQ(result.rollouts, 1000);
}

TEST(EquityExact, CertainLoss) {
    const auto board = board_of({"Qs", "Js", "Ts", "2c", "3d"});
    const std::array<HoleCards, 1> villain{hole("As", "Ks")};
    const auto result = equity_vs_hands(hole("Ah", "Ad"), board, villain, opts(1000, 1));
    EXPECT_DOUBLE_EQ(result.equity, 0.0);
    EXPECT_DOUBLE_EQ(result.win, 0.0);
    EXPECT_DOUBLE_EQ(result.tie, 0.0);
}

TEST(EquityExact, TwoWayChop) {
    const auto board = board_of({"Qs", "Js", "Ts", "2c", "3d"});
    const std::array<HoleCards, 1> villain{hole("Ac", "Kc")};
    const auto result = equity_vs_hands(hole("Ad", "Kd"), board, villain, opts(1000, 1));
    EXPECT_DOUBLE_EQ(result.equity, 0.5);
    EXPECT_DOUBLE_EQ(result.win, 0.0);
    EXPECT_DOUBLE_EQ(result.tie, 1.0);
}

TEST(EquityExact, ThreeWayChopOnABoardThatPlays) {
    const auto board = board_of({"Ts", "9s", "8s", "7s", "6s"});
    const std::array<HoleCards, 2> villains{hole("2c", "3c"), hole("4h", "5h")};
    const auto result = equity_vs_hands(hole("Ad", "Kd"), board, villains, opts(1000, 1));
    EXPECT_NEAR(result.equity, 1.0 / 3.0, 1e-12);
    EXPECT_DOUBLE_EQ(result.tie, 1.0);
}

TEST(EquityExact, SixWayChopVersusRandomHands) {
    // A royal on board plays for everyone, whatever they hold.
    const auto board = board_of({"As", "Ks", "Qs", "Js", "Ts"});
    const auto result = equity_vs_random(hole("2c", "3d"), board, 5, opts(1000, 1));
    EXPECT_NEAR(result.equity, 1.0 / 6.0, 1e-12);
    EXPECT_DOUBLE_EQ(result.tie, 1.0);
}

TEST(EquityExact, TieShareSplitsOnlyAmongTiedPlayers) {
    // Hero and seat 1 chop with Ace-King over a third player they both beat.
    const auto board = board_of({"Qs", "Js", "Ts", "2c", "3d"});
    const std::array<HoleCards, 2> villains{hole("Ac", "Kc"), hole("4h", "4d")};
    const auto result = equity_vs_hands(hole("Ad", "Kd"), board, villains, opts(1000, 1));
    EXPECT_DOUBLE_EQ(result.equity, 0.5);
}

TEST(EquityExact, NutsVersusAnyRandomHands) {
    const auto board = board_of({"Qs", "Js", "Ts", "2c", "3d"});
    const auto result = equity_vs_random(hole("As", "Ks"), board, 5, opts(2000, 3));
    EXPECT_DOUBLE_EQ(result.equity, 1.0);
}

// --- Statistical behaviour ---------------------------------------------------

TEST(EquityStats, StandardErrorShrinksWithSquareRootOfRollouts) {
    const auto small = equity_vs_random(hole("Qs", "Jd"), {}, 2, opts(25'000, 7));
    const auto large = equity_vs_random(hole("Qs", "Jd"), {}, 2, opts(400'000, 8));
    EXPECT_NEAR(small.std_error / large.std_error, 4.0, 0.2);
}

TEST(EquityStats, ReportedStandardErrorMatchesObservedSpread) {
    constexpr int kRuns = 200;
    double sum = 0.0;
    double sum_squares = 0.0;
    double reported = 0.0;
    for (int run = 0; run < kRuns; ++run) {
        const auto result = equity_vs_random(hole("Qs", "Jd"), {}, 1, opts(2000, static_cast<std::uint64_t>(run)));
        sum += result.equity;
        sum_squares += result.equity * result.equity;
        reported += result.std_error;
    }
    const double mean = sum / kRuns;
    const double observed = std::sqrt((sum_squares - kRuns * mean * mean) / (kRuns - 1));
    // The sample deviation over 200 runs carries about 5% error of its own.
    EXPECT_NEAR(observed / (reported / kRuns), 1.0, 0.2);
}

// Five chunks, so chunks repeating each other's streams would show up here as a
// spread about 2.2 times what the standard error reports. Compared with the exact
// value, to leave no room for a shared bias to cancel.
TEST(EquityStats, ReportedStandardErrorHoldsAcrossChunks) {
    const HoleCards hero = hole("As", "Kd");
    const HoleCards villain = hole("Qh", "Qc");
    const auto board = board_of({"7c", "4h", "2d"});
    const double exact = exact_heads_up(hero, villain, board);
    const std::array<HoleCards, 1> villains{villain};

    constexpr int kRollouts = 5 * kEquityChunkRollouts - 100;
    constexpr int kRuns = 200;
    double sum_squares = 0.0;
    double reported = 0.0;
    for (int run = 0; run < kRuns; ++run) {
        const auto result = equity_vs_hands(hero, board, villains, opts(kRollouts, static_cast<std::uint64_t>(run) + 1));
        sum_squares += (result.equity - exact) * (result.equity - exact);
        reported += result.std_error;
    }
    EXPECT_NEAR(std::sqrt(sum_squares / kRuns) / (reported / kRuns), 1.0, 0.2);
}

TEST(EquityStats, WinTieAndEquityAreConsistent) {
    const auto result = equity_vs_random(hole("Kh", "Qd"), board_of({"Ks", "7c", "2d"}), 3, opts(50'000, 5));
    EXPECT_GE(result.equity, result.win);
    EXPECT_LE(result.equity, result.win + result.tie / 2.0 + 1e-12);
    EXPECT_GE(result.equity, result.win + result.tie / 4.0 - 1e-12);
    EXPECT_LE(result.win + result.tie, 1.0);
}

TEST(EquityStats, MoreOpponentsReduceEquity) {
    double previous = 1.0;
    for (int opponents = 1; opponents <= 5; ++opponents) {
        const auto result = equity_vs_random(hole("Qs", "Qd"), {}, opponents, opts(100'000, 11));
        EXPECT_LT(result.equity, previous) << opponents;
        previous = result.equity;
    }
}

TEST(EquityStats, DefaultRolloutsAreFiveThousand) {
    const auto result = equity_vs_random(hole("Qs", "Qd"), {}, 1);
    EXPECT_EQ(result.rollouts, 5000);
}

// --- Determinism -------------------------------------------------------------

void expect_identical(const EquityResult& a, const EquityResult& b) {
    EXPECT_EQ(a.win, b.win);
    EXPECT_EQ(a.tie, b.tie);
    EXPECT_EQ(a.equity, b.equity);
    EXPECT_EQ(a.std_error, b.std_error);
    EXPECT_EQ(a.rollouts, b.rollouts);
}

TEST(EquityDeterminism, ResultIsIdenticalForAnyThreadCount) {
    const HoleCards hero = hole("Js", "Th");
    const auto board = board_of({"9c", "8c", "2d"});
    for (const int rollouts : {1, 4095, 4096, 4097, 5000, 50'000, 150'000}) {
        const auto one = equity_vs_random(hero, board, 3, opts(rollouts, 42, 1));
        for (const int threads : {0, 2, 3, 8, 64}) {
            SCOPED_TRACE(testing::Message() << rollouts << " rollouts, " << threads << " threads");
            expect_identical(one, equity_vs_random(hero, board, 3, opts(rollouts, 42, threads)));
        }
    }
}

TEST(EquityDeterminism, KnownHandsAreAlsoThreadIndependent) {
    const std::array<HoleCards, 1> villain{hole("Kd", "Kc")};
    expect_identical(equity_vs_hands(hole("As", "Ah"), {}, villain, opts(60'000, 9, 1)),
                     equity_vs_hands(hole("As", "Ah"), {}, villain, opts(60'000, 9, 6)));
}

TEST(EquityDeterminism, SameSeedRepeatsAndDifferentSeedsDiffer) {
    const HoleCards hero = hole("Js", "Th");
    const auto a = equity_vs_random(hero, {}, 2, opts(5000, 1));
    const auto b = equity_vs_random(hero, {}, 2, opts(5000, 1));
    const auto c = equity_vs_random(hero, {}, 2, opts(5000, 2));
    expect_identical(a, b);
    EXPECT_NE(a.equity, c.equity);
}

// --- Validation --------------------------------------------------------------

TEST(EquityValidation, RejectsBadBoardSizes) {
    const HoleCards hero = hole("As", "Ad");
    EXPECT_THROW(equity_vs_random(hero, board_of({"2c"}), 1), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, board_of({"2c", "3c"}), 1), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, board_of({"2c", "3c", "4c", "5c", "6c", "7c"}), 1), std::invalid_argument);
}

TEST(EquityValidation, RejectsBadOpponentCounts) {
    const HoleCards hero = hole("As", "Ad");
    EXPECT_THROW(equity_vs_random(hero, {}, 0), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, {}, 6), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, {}, -1), std::invalid_argument);

    EXPECT_THROW(equity_vs_hands(hero, {}, {}), std::invalid_argument);
    const std::array<HoleCards, 6> six{hole("2c", "2d"), hole("3c", "3d"), hole("4c", "4d"),
                                       hole("5c", "5d"), hole("6c", "6d"), hole("7c", "7d")};
    EXPECT_THROW(equity_vs_hands(hero, {}, six), std::invalid_argument);
}

TEST(EquityValidation, RejectsDuplicateCards) {
    EXPECT_THROW(equity_vs_random(hole("As", "As"), {}, 1), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hole("As", "Ad"), board_of({"As", "2c", "3c"}), 1), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hole("Ah", "Ad"), board_of({"2c", "2c", "3c"}), 1), std::invalid_argument);

    const std::array<HoleCards, 1> clash_hero{hole("As", "2d")};
    EXPECT_THROW(equity_vs_hands(hole("As", "Ad"), {}, clash_hero), std::invalid_argument);
    const std::array<HoleCards, 1> clash_board{hole("2c", "2d")};
    EXPECT_THROW(equity_vs_hands(hole("As", "Ad"), board_of({"2c", "3c", "4c"}), clash_board), std::invalid_argument);
    const std::array<HoleCards, 2> clash_each_other{hole("2c", "2d"), hole("2c", "3d")};
    EXPECT_THROW(equity_vs_hands(hole("As", "Ad"), {}, clash_each_other), std::invalid_argument);
}

TEST(EquityValidation, RejectsInvalidCardIds) {
    const HoleCards bad{Card(52), card("Ad")};
    EXPECT_THROW(equity_vs_random(bad, {}, 1), std::invalid_argument);
}

TEST(EquityValidation, RejectsBadOptions) {
    const HoleCards hero = hole("As", "Ad");
    EXPECT_THROW(equity_vs_random(hero, {}, 1, opts(0, 1)), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, {}, 1, opts(-5, 1)), std::invalid_argument);
    EXPECT_THROW(equity_vs_random(hero, {}, 1, opts(100, 1, -1)), std::invalid_argument);
}

// --- Timing, run by hand -----------------------------------------------------
// core_tests --gtest_also_run_disabled_tests --gtest_filter=*EquityBench*

TEST(EquityBench, DISABLED_TimePerFiveThousandRollouts) {
    using Clock = std::chrono::steady_clock;
    const HoleCards hero = hole("Js", "Th");
    const auto board = board_of({"9c", "8c", "2d"});

    const auto time_ms = [&](int opponents, int rollouts, int threads) {
        constexpr int kRepeats = 50;
        double sink = 0.0;
        const auto start = Clock::now();
        for (int i = 0; i < kRepeats; ++i) {
            sink += equity_vs_random(hero, board, opponents, opts(rollouts, static_cast<std::uint64_t>(i), threads)).equity;
        }
        const std::chrono::duration<double, std::milli> elapsed = Clock::now() - start;
        std::printf("opponents=%d rollouts=%d threads=%d: %.3f ms (mean equity %.3f)\n", opponents, rollouts, threads,
                    elapsed.count() / kRepeats, sink / kRepeats);
    };

    for (const int threads : {1, 2, 4, 8}) {
        time_ms(1, 5000, threads);
        time_ms(5, 5000, threads);
    }
    for (const int rollouts : {20'000, 50'000, 100'000, 400'000}) {
        for (const int threads : {1, 4, 8}) time_ms(3, rollouts, threads);
    }
}
