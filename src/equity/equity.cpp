#include "equity/equity.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "core/deck.h"
#include "core/hand_evaluator.h"

namespace poker {
namespace {

// Hero's pot share is tallied in 60ths, which divides evenly among 2-6 players.
// Integer totals make the sum exact and independent of the order chunks finish in.
constexpr std::uint64_t kShareUnits = 60;

struct Tally {
    std::uint64_t wins = 0;
    std::uint64_t ties = 0;
    std::uint64_t share = 0;
    std::uint64_t share_squared = 0;
};

struct Scenario {
    HoleCards hero{};
    std::array<Card, 5> board{};
    int board_size = 0;
    std::array<HoleCards, kMaxOpponents> known{};
    int known_count = 0;
    int random_count = 0;
    Deck deck;
};

Scenario make_scenario(const HoleCards& hero, std::span<const Card> board, std::span<const HoleCards> known,
                       int random_count, const EquityOptions& options) {
    if (options.rollouts <= 0) throw std::invalid_argument("equity: rollouts must be positive");
    if (options.threads < 0) throw std::invalid_argument("equity: threads must not be negative");
    if (board.size() != 0 && board.size() != 3 && board.size() != 4 && board.size() != 5) {
        throw std::invalid_argument("equity: board must have 0, 3, 4 or 5 cards");
    }
    const int opponents = static_cast<int>(known.size()) + random_count;
    if (opponents < 1 || opponents > kMaxOpponents) {
        throw std::invalid_argument("equity: need 1 to 5 opponents");
    }

    std::vector<Card> used(hero.begin(), hero.end());
    used.insert(used.end(), board.begin(), board.end());
    for (const HoleCards& hole : known) used.insert(used.end(), hole.begin(), hole.end());

    std::bitset<kDeckSize> seen;
    for (const Card card : used) {
        if (card.id >= kDeckSize || seen.test(card.id)) {
            throw std::invalid_argument("equity: duplicate or invalid card");
        }
        seen.set(card.id);
    }

    Scenario scenario;
    scenario.hero = hero;
    std::copy(board.begin(), board.end(), scenario.board.begin());
    scenario.board_size = static_cast<int>(board.size());
    std::copy(known.begin(), known.end(), scenario.known.begin());
    scenario.known_count = static_cast<int>(known.size());
    scenario.random_count = random_count;
    scenario.deck = Deck(std::span<const Card>(used));
    return scenario;
}

// SplitMix64: decorrelates the nearby inputs that chunk seeds are built from.
std::uint64_t chunk_seed(std::uint64_t seed, std::uint64_t chunk) {
    std::uint64_t z = seed + (chunk + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

Tally run_chunk(const Scenario& scenario, std::uint64_t seed, int rollouts) {
    std::mt19937_64 rng(seed);
    Tally tally;

    for (int rollout = 0; rollout < rollouts; ++rollout) {
        Deck deck = scenario.deck;
        std::array<Card, 5> board = scenario.board;
        for (int i = scenario.board_size; i < 5; ++i) board[i] = deck.deal_random(rng);

        const HandRank hero_rank = evaluate7(scenario.hero, board);
        HandRank best = kWorstHandRank + 1;
        int at_best = 0;
        const auto score = [&](const HoleCards& hole) {
            const HandRank rank = evaluate7(hole, board);
            if (rank < best) {
                best = rank;
                at_best = 0;
            }
            if (rank == best) ++at_best;
        };

        for (int i = 0; i < scenario.known_count; ++i) score(scenario.known[i]);
        for (int i = 0; i < scenario.random_count; ++i) {
            const Card first = deck.deal_random(rng);
            const Card second = deck.deal_random(rng);
            score(HoleCards{first, second});
        }

        std::uint64_t share = 0;
        if (hero_rank < best) {
            ++tally.wins;
            share = kShareUnits;
        } else if (hero_rank == best) {
            ++tally.ties;
            share = kShareUnits / static_cast<std::uint64_t>(at_best + 1);
        }
        tally.share += share;
        tally.share_squared += share * share;
    }
    return tally;
}

int thread_count(const EquityOptions& options, int chunks) {
    int wanted = options.threads;
    if (wanted == 0) {
        wanted = options.rollouts < kEquityParallelThreshold
                     ? 1
                     : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }
    return std::min(wanted, chunks);
}

EquityResult run(const Scenario& scenario, const EquityOptions& options) {
    const int chunks = (options.rollouts - 1) / kEquityChunkRollouts + 1;
    std::vector<Tally> tallies(static_cast<std::size_t>(chunks));
    std::atomic<int> next_chunk{0};

    const auto work = [&] {
        for (int chunk = next_chunk.fetch_add(1); chunk < chunks; chunk = next_chunk.fetch_add(1)) {
            const int rollouts = chunk == chunks - 1 ? options.rollouts - chunk * kEquityChunkRollouts
                                                     : kEquityChunkRollouts;
            tallies[static_cast<std::size_t>(chunk)] =
                run_chunk(scenario, chunk_seed(options.seed, static_cast<std::uint64_t>(chunk)), rollouts);
        }
    };

    {
        std::vector<std::jthread> helpers;
        const int threads = thread_count(options, chunks);
        helpers.reserve(static_cast<std::size_t>(threads - 1));
        try {
            for (int i = 1; i < threads; ++i) helpers.emplace_back(work);
        } catch (const std::system_error&) {
            // Out of threads: the ones running, and this one, still finish every chunk.
        }
        work();
    }

    Tally total;
    for (const Tally& tally : tallies) {
        total.wins += tally.wins;
        total.ties += tally.ties;
        total.share += tally.share;
        total.share_squared += tally.share_squared;
    }

    const double n = static_cast<double>(options.rollouts);
    const double units = static_cast<double>(kShareUnits);
    const double mean = static_cast<double>(total.share) / units / n;
    const double mean_square = static_cast<double>(total.share_squared) / (units * units) / n;

    EquityResult result;
    result.win = static_cast<double>(total.wins) / n;
    result.tie = static_cast<double>(total.ties) / n;
    result.equity = mean;
    result.rollouts = options.rollouts;
    if (options.rollouts > 1) {
        const double variance = std::max(0.0, mean_square - mean * mean) * n / (n - 1.0);
        result.std_error = std::sqrt(variance / n);
    }
    return result;
}

}  // namespace

EquityResult equity_vs_random(const HoleCards& hero, std::span<const Card> board, int opponents,
                              const EquityOptions& options) {
    return run(make_scenario(hero, board, {}, opponents, options), options);
}

EquityResult equity_vs_hands(const HoleCards& hero, std::span<const Card> board,
                             std::span<const HoleCards> opponents, const EquityOptions& options) {
    return run(make_scenario(hero, board, opponents, 0, options), options);
}

}  // namespace poker
