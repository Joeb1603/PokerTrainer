// Generates src/range/generated/preflop_equity_data.cpp: all-in preflop equity of
// every hand class against every other, by Monte Carlo.
//
//   gen_preflop_tables [--rollouts N] [--threads N] [--out PATH]
//
// Each class pair has its own random stream, seeded from kSeed and the pair's
// index, and tallies are integers, so the output is identical for any thread count.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/deck.h"
#include "core/hand_evaluator.h"
#include "range/combos.h"
#include "range/preflop_matrix.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

constexpr std::uint64_t kSeed = 20261106;
constexpr int kDefaultTableRollouts = 200'000;
constexpr int kValuesPerLine = 16;

struct Options {
    int rollouts = kDefaultTableRollouts;
    int threads = 0;
    std::string out = "src/range/generated/preflop_equity_data.cpp";
};

// SplitMix64, as in equity.cpp: decorrelates seeds built from nearby pair indices.
std::uint64_t pair_seed(std::uint64_t seed, std::uint64_t pair) {
    std::uint64_t z = seed + (pair + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Unbiased integer in [0, bound) from raw generator bits, the same on every
// standard library.
std::uint64_t bounded(std::mt19937_64& rng, std::uint64_t bound) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t limit = kMax - (kMax % bound + 1) % bound;
    std::uint64_t value = rng();
    while (value > limit) value = rng();
    return value % bound;
}

// Hero's pot share in halves: 2 for a win, 1 for a split.
std::uint64_t rollout_pair(const ClassCombos& hero, const ClassCombos& villain, int rollouts, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uint64_t halves = 0;
    for (int rollout = 0; rollout < rollouts; ++rollout) {
        // Rejection keeps the pair uniform over the combos that share no card.
        int a = 0;
        int b = 0;
        do {
            a = hero.combos[static_cast<std::size_t>(bounded(rng, static_cast<std::uint64_t>(hero.count)))];
            b = villain.combos[static_cast<std::size_t>(bounded(rng, static_cast<std::uint64_t>(villain.count)))];
        } while ((combo_mask(a) & combo_mask(b)) != 0);

        const std::array<Card, 2> mine = combo_cards(a);
        const std::array<Card, 2> theirs = combo_cards(b);
        const std::array<Card, 4> used{mine[0], mine[1], theirs[0], theirs[1]};
        Deck deck{std::span<const Card>(used)};
        std::array<Card, 5> board{};
        for (Card& card : board) card = deck.deal_random(rng);

        const HandRank my_rank = evaluate7(mine, board);
        const HandRank their_rank = evaluate7(theirs, board);
        if (my_rank < their_rank) halves += 2;
        if (my_rank == their_rank) halves += 1;
    }
    return halves;
}

bool parse_int(std::string_view text, int& value) {
    try {
        std::size_t used = 0;
        value = std::stoi(std::string(text), &used);
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) return false;
        const std::string_view value = argv[++i];
        if (arg == "--rollouts") {
            if (!parse_int(value, options.rollouts) || options.rollouts <= 0) return false;
        } else if (arg == "--threads") {
            if (!parse_int(value, options.threads) || options.threads < 0) return false;
        } else if (arg == "--out") {
            options.out = std::string(value);
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "usage: gen_preflop_tables [--rollouts N] [--threads N] [--out PATH]\n";
        return 2;
    }

    std::array<HandClass, kHandClassCount> by_index{};
    for (const HandClass hand : hand_classes_by_strength()) by_index[static_cast<std::size_t>(hand.index())] = hand;

    struct Pair {
        int i = 0;
        int j = 0;
    };
    std::vector<Pair> pairs;
    pairs.reserve(detail::kPreflopTriangleSize);
    for (int i = 0; i < kHandClassCount; ++i) {
        for (int j = i; j < kHandClassCount; ++j) pairs.push_back({i, j});
    }

    std::vector<std::uint64_t> halves(pairs.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex print_mutex;
    const auto start = std::chrono::steady_clock::now();

    const auto work = [&] {
        for (std::size_t p = next.fetch_add(1); p < pairs.size(); p = next.fetch_add(1)) {
            const HandClass hero = by_index[static_cast<std::size_t>(pairs[p].i)];
            const HandClass villain = by_index[static_cast<std::size_t>(pairs[p].j)];
            halves[p] = rollout_pair(class_combos(hero), class_combos(villain), options.rollouts, pair_seed(kSeed, p));
            const std::size_t finished = done.fetch_add(1) + 1;
            if (finished % 1000 == 0) {
                const std::lock_guard lock(print_mutex);
                std::cerr << finished << " / " << pairs.size() << " pairs\n";
            }
        }
    };

    int threads = options.threads;
    if (threads == 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    {
        std::vector<std::jthread> workers;
        for (int t = 1; t < threads; ++t) workers.emplace_back(work);
        work();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ofstream out(options.out);
    if (!out) {
        std::cerr << "cannot write " << options.out << '\n';
        return 1;
    }
    out << "// Generated by tools/gen_preflop_tables.cpp --rollouts " << options.rollouts << ", seed " << kSeed
        << ". Do not edit.\n"
        << "// Hero's all-in equity * 65535 for each class pair (i <= j by HandClass::index()),\n"
        << "// packed as described in range/preflop_matrix.h.\n"
        << "#include \"range/preflop_matrix.h\"\n\n"
        << "namespace poker::detail {\n\n"
        << "const int kPreflopEquityRollouts = " << options.rollouts << ";\n\n"
        << "const std::uint16_t kPreflopEquity[kPreflopTriangleSize] = {\n";
    const double denominator = 2.0 * static_cast<double>(options.rollouts);
    for (std::size_t p = 0; p < pairs.size(); ++p) {
        const double equity = static_cast<double>(halves[p]) / denominator;
        const long scaled = std::lround(equity * 65535.0);
        out << (p % kValuesPerLine == 0 ? "    " : " ") << scaled << ',';
        if (p % kValuesPerLine == kValuesPerLine - 1 || p + 1 == pairs.size()) out << '\n';
    }
    out << "};\n\n}  // namespace poker::detail\n";
    if (!out) {
        std::cerr << "write failed: " << options.out << '\n';
        return 1;
    }

    std::cerr << "wrote " << options.out << ": " << pairs.size() << " pairs x " << options.rollouts
              << " rollouts in " << seconds << " s on " << threads << " threads; standard error <= "
              << 100.0 * 0.5 / std::sqrt(static_cast<double>(options.rollouts)) << " pp\n";
    return 0;
}
