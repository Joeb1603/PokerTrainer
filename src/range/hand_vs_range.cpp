#include "range/hand_vs_range.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "core/deck.h"
#include "core/hand_evaluator.h"
#include "range/preflop_matrix.h"

namespace poker {
namespace {

// Hero's pot share is tallied in 60ths, which divides evenly among 2-6 players,
// as in equity.cpp. Integer totals are exact in any summation order.
constexpr std::uint64_t kShareUnits = 60;

// A multiway rollout that needs more whole-deal redraws than this gives up: the
// ranges can almost never be dealt together, so the result would mean little.
constexpr int kMaxDealAttempts = 1 << 14;

// Combos below which an automatic flop runs inline: about 0.5 ms of work. A
// full flop vector takes about 8 ms on one thread and 2.5 ms on four
// (HandVsRangeBench).
constexpr int kParallelFlopCombos = 64;

// Multiway rollouts from which automatic threading splits chunks across
// threads: at 3,000 rollouts (three chunks) four threads took 0.4 ms against
// 0.9 ms inline in HandVsRangeBench.
constexpr int kParallelMultiwayRollouts = 2 * kRangeChunkRollouts;

// SplitMix64, as equity.cpp: decorrelates nearby inputs such as chunk numbers.
std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    std::uint64_t z = seed + (value + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Checks the cards and returns the mask of hero and board.
CardMask validate_cards(const HoleCards& hero, std::span<const Card> board, int threads, const char* what) {
    if (threads < 0) throw std::invalid_argument(std::string(what) + ": threads must not be negative");
    if (board.size() != 0 && board.size() != 3 && board.size() != 4 && board.size() != 5) {
        throw std::invalid_argument(std::string(what) + ": board must have 0, 3, 4 or 5 cards");
    }
    CardMask dead = 0;
    const auto add = [&](Card card) {
        if (card.id >= kDeckSize || (dead & card_mask(card)) != 0) {
            throw std::invalid_argument(std::string(what) + ": duplicate or invalid card");
        }
        dead |= card_mask(card);
    };
    for (const Card card : hero) add(card);
    for (const Card card : board) add(card);
    return dead;
}

// Runs work(item, worker) for each item in [0, items) on up to `threads`
// threads, worker in [0, threads). Returns how many workers there were.
template <typename Work>
int run_parallel(int items, int threads, const Work& work) {
    threads = std::clamp(threads, 1, std::max(1, items));
    std::atomic<int> next{0};
    // An exception escaping a helper thread would call std::terminate, so each
    // worker keeps the first one and it is rethrown here once all have joined.
    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto worker = [&](int id) {
        try {
            for (int item = next.fetch_add(1); item < items; item = next.fetch_add(1)) work(item, id);
        } catch (...) {
            const std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
            next.store(items);  // Hand out no more work
        }
    };
    {
        std::vector<std::jthread> helpers;
        helpers.reserve(static_cast<std::size_t>(threads - 1));
        try {
            for (int id = 1; id < threads; ++id) helpers.emplace_back(worker, id);
        } catch (const std::system_error&) {
            // Out of threads: the ones running, and this one, still finish every item.
        }
        worker(0);
    }
    if (failure) std::rethrow_exception(failure);
    return threads;
}

int hardware_threads() { return static_cast<int>(std::max(1u, std::thread::hardware_concurrency())); }

// ---- Heads-up, per combo ----------------------------------------------------

struct ActiveCombo {
    HoleCards cards{};
    CardMask mask = 0;
    std::uint16_t index = 0;
};

// Per combo: 2 * wins + ties, and the number of runouts scored.
struct ComboTally {
    std::vector<std::uint32_t> doubled_wins;
    std::vector<std::uint32_t> runouts;

    explicit ComboTally(std::size_t combos) : doubled_wins(combos, 0), runouts(combos, 0) {}

    void score(std::size_t combo, HandRank hero, HandRank villain) {
        doubled_wins[combo] += hero < villain ? 2u : hero == villain ? 1u : 0u;
        ++runouts[combo];
    }
};

// Every card not in `dead`, in id order.
std::vector<Card> unseen_cards(CardMask dead) {
    std::vector<Card> cards;
    for (int id = 0; id < kDeckSize; ++id) {
        if ((dead & (CardMask{1} << id)) == 0) cards.emplace_back(id);
    }
    return cards;
}

void score_river(const HoleCards& hero, const std::array<Card, 5>& board, std::span<const ActiveCombo> combos,
                 ComboTally& tally) {
    const HandRank hero_rank = evaluate7(hero, board);
    for (std::size_t i = 0; i < combos.size(); ++i) tally.score(i, hero_rank, evaluate7(combos[i].cards, board));
}

// The turn's river cards, one per item.
void score_turn(const HoleCards& hero, const std::array<Card, 5>& turn_board, std::span<const Card> rivers,
                std::span<const ActiveCombo> combos, int threads, ComboTally& total) {
    const int items = static_cast<int>(rivers.size());
    // One tally per worker that can run, not per thread requested.
    threads = std::clamp(threads, 1, std::max(1, items));
    std::vector<ComboTally> tallies(static_cast<std::size_t>(threads), ComboTally(combos.size()));
    const int workers = run_parallel(items, threads, [&](int item, int worker) {
        const Card river = rivers[static_cast<std::size_t>(item)];
        std::array<Card, 5> board = turn_board;
        board[4] = river;
        const HandRank hero_rank = evaluate7(hero, board);
        ComboTally& tally = tallies[static_cast<std::size_t>(worker)];
        for (std::size_t i = 0; i < combos.size(); ++i) {
            if ((combos[i].mask & card_mask(river)) != 0) continue;
            tally.score(i, hero_rank, evaluate7(combos[i].cards, board));
        }
    });
    for (int w = 0; w < workers; ++w) {
        const ComboTally& tally = tallies[static_cast<std::size_t>(w)];
        for (std::size_t i = 0; i < combos.size(); ++i) {
            total.doubled_wins[i] += tally.doubled_wins[i];
            total.runouts[i] += tally.runouts[i];
        }
    }
}

// The balanced flop design described in the header, one turn position per item.
void score_flop(const HoleCards& hero, const std::array<Card, 5>& flop_board, CardMask dead, std::uint64_t seed,
                int rivers_per_turn, std::span<const ActiveCombo> combos, int threads, ComboTally& total) {
    std::vector<Card> unseen = unseen_cards(dead);
    const int n = static_cast<int>(unseen.size());  // 47
    const bool exact = rivers_per_turn == 0 || rivers_per_turn >= kExactFlopRiversPerTurn;
    const int per_turn = exact ? kExactFlopRiversPerTurn : rivers_per_turn;
    // Exact enumeration counts each runout once in any order; a sample shuffles
    // the order so that which runouts it takes is random.
    if (!exact) {
        std::vector<Card> known;
        for (int id = 0; id < kDeckSize; ++id) {
            if ((dead & (CardMask{1} << id)) != 0) known.emplace_back(id);
        }
        Deck order(known);
        std::mt19937_64 rng(mix(seed, 0));
        order.shuffle(rng);
        for (Card& card : unseen) card = order.deal();
    }
    // Rivers a turn may need: its K, plus up to two that a combo holds.
    const int window = exact ? per_turn : std::min(per_turn + 2, n - 1);

    threads = std::clamp(threads, 1, n);
    std::vector<ComboTally> tallies(static_cast<std::size_t>(threads), ComboTally(combos.size()));
    const int workers = run_parallel(n, threads, [&](int turn_pos, int worker) {
        const Card turn = unseen[static_cast<std::size_t>(turn_pos)];
        std::array<std::array<Card, 5>, kExactFlopRiversPerTurn + 2> boards{};
        std::array<CardMask, kExactFlopRiversPerTurn + 2> river_masks{};
        std::array<HandRank, kExactFlopRiversPerTurn + 2> hero_ranks{};
        for (int s = 0; s < window; ++s) {
            const Card river = unseen[static_cast<std::size_t>((turn_pos + 1 + s) % n)];
            auto& board = boards[static_cast<std::size_t>(s)];
            board = flop_board;
            board[3] = turn;
            board[4] = river;
            river_masks[static_cast<std::size_t>(s)] = card_mask(river);
            hero_ranks[static_cast<std::size_t>(s)] = evaluate7(hero, board);
        }

        ComboTally& tally = tallies[static_cast<std::size_t>(worker)];
        for (std::size_t i = 0; i < combos.size(); ++i) {
            const ActiveCombo& combo = combos[i];
            if ((combo.mask & card_mask(turn)) != 0) continue;
            int taken = 0;
            for (int s = 0; s < window && taken < per_turn; ++s) {
                if ((combo.mask & river_masks[static_cast<std::size_t>(s)]) != 0) continue;
                tally.score(i, hero_ranks[static_cast<std::size_t>(s)],
                            evaluate7(combo.cards, boards[static_cast<std::size_t>(s)]));
                // Exact enumeration skips a held river; a sample replaces it.
                if (!exact) ++taken;
            }
        }
    });
    for (int w = 0; w < workers; ++w) {
        const ComboTally& tally = tallies[static_cast<std::size_t>(w)];
        for (std::size_t i = 0; i < combos.size(); ++i) {
            total.doubled_wins[i] += tally.doubled_wins[i];
            total.runouts[i] += tally.runouts[i];
        }
    }
}

// ---- Multiway ---------------------------------------------------------------

// One villain's combos with positive weight that hero and the board leave
// dealable, with running weight totals for sampling in proportion to weight.
struct VillainTable {
    std::vector<std::uint16_t> combos;
    std::vector<double> cumulative;

    std::uint16_t sample(std::mt19937_64& rng) const {
        // 53 random bits, as a double in [0, 1): the same on every platform.
        const double u = static_cast<double>(rng() >> 11) * 0x1.0p-53 * cumulative.back();
        const auto it = std::upper_bound(cumulative.begin(), cumulative.end(), u);
        const auto at = std::min(static_cast<std::size_t>(it - cumulative.begin()), combos.size() - 1);
        return combos[at];
    }
};

struct MultiwayScenario {
    HoleCards hero{};
    std::array<Card, 5> board{};
    int board_size = 0;
    Deck deck;  // Without hero's and the board's cards
    std::vector<VillainTable> villains;
    std::vector<std::vector<int>> layer_villains;  // Indices into `villains`
};

struct LayerTally {
    std::uint64_t share = 0;
    std::uint64_t share_squared = 0;
};

struct MultiwayChunk {
    std::vector<LayerTally> layers;
    std::uint64_t restarts = 0;
    bool gave_up = false;
};

MultiwayChunk run_multiway_chunk(const MultiwayScenario& scenario, std::uint64_t seed, int rollouts) {
    std::mt19937_64 rng(seed);
    MultiwayChunk chunk;
    chunk.layers.resize(scenario.layer_villains.size());
    const std::size_t villain_count = scenario.villains.size();
    std::array<std::uint16_t, kMaxSeats> dealt{};
    std::array<HandRank, kMaxSeats> ranks{};

    for (int rollout = 0; rollout < rollouts; ++rollout) {
        CardMask held = 0;
        for (int attempt = 0;; ++attempt) {
            if (attempt == kMaxDealAttempts) {
                chunk.gave_up = true;
                return chunk;
            }
            held = 0;
            bool clash = false;
            for (std::size_t v = 0; v < villain_count && !clash; ++v) {
                dealt[v] = scenario.villains[v].sample(rng);
                const CardMask mask = combo_mask(dealt[v]);
                clash = (held & mask) != 0;
                held |= mask;
            }
            if (!clash) break;
            ++chunk.restarts;
        }

        Deck deck = scenario.deck;
        std::array<Card, 5> board = scenario.board;
        for (int i = scenario.board_size; i < 5; ++i) {
            Card next = deck.deal_random(rng);
            while ((held & card_mask(next)) != 0) next = deck.deal_random(rng);
            board[static_cast<std::size_t>(i)] = next;
        }

        const HandRank hero_rank = evaluate7(scenario.hero, board);
        for (std::size_t v = 0; v < villain_count; ++v) ranks[v] = evaluate7(combo_cards(dealt[v]), board);

        for (std::size_t layer = 0; layer < scenario.layer_villains.size(); ++layer) {
            HandRank best = kWorstHandRank + 1;
            int at_best = 0;
            for (const int v : scenario.layer_villains[layer]) {
                const HandRank rank = ranks[static_cast<std::size_t>(v)];
                if (rank < best) {
                    best = rank;
                    at_best = 0;
                }
                if (rank == best) ++at_best;
            }
            std::uint64_t share = 0;
            if (hero_rank < best) {
                share = kShareUnits;
            } else if (hero_rank == best) {
                share = kShareUnits / static_cast<std::uint64_t>(at_best + 1);
            }
            chunk.layers[layer].share += share;
            chunk.layers[layer].share_squared += share * share;
        }
    }
    return chunk;
}

}  // namespace

ComboEquities hero_vs_each_combo(const HoleCards& hero, std::span<const Card> board,
                                 const std::bitset<kComboCount>& needed, const RangeEquityOptions& options) {
    const CardMask dead = validate_cards(hero, board, options.threads, "hero_vs_each_combo");
    if (options.flop_rivers_per_turn < 0) {
        throw std::invalid_argument("hero_vs_each_combo: flop_rivers_per_turn must not be negative");
    }

    ComboEquities result;
    result.dead_cards = dead;

    std::vector<ActiveCombo> combos;
    combos.reserve(needed.count());
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (!needed.test(static_cast<std::size_t>(combo)) || (combo_mask(combo) & dead) != 0) continue;
        combos.push_back({combo_cards(combo), combo_mask(combo), static_cast<std::uint16_t>(combo)});
    }

    if (board.empty()) {
        const HandClass hero_class = HandClass::from_cards(hero[0], hero[1]);
        for (const ActiveCombo& combo : combos) {
            result.equity[combo.index] = static_cast<float>(class_vs_class_equity(hero_class, combo_class(combo.index)));
            result.valid.set(combo.index);
        }
        return result;
    }

    std::array<Card, 5> full{};
    std::copy(board.begin(), board.end(), full.begin());
    ComboTally tally(combos.size());
    const int requested = options.threads;
    switch (board.size()) {
        case 5:
            score_river(hero, full, combos, tally);
            break;
        case 4: {
            // About 45,000 evaluations, 0.9 ms inline: four threads save only
            // 0.4 ms (HandVsRangeBench), not worth starting them for.
            const std::vector<Card> rivers = unseen_cards(dead);
            score_turn(hero, full, rivers, combos, requested == 0 ? 1 : requested, tally);
            break;
        }
        default: {
            const int automatic = combos.size() < kParallelFlopCombos ? 1 : hardware_threads();
            score_flop(hero, full, dead, options.seed, options.flop_rivers_per_turn, combos,
                       requested == 0 ? automatic : requested, tally);
            break;
        }
    }

    for (std::size_t i = 0; i < combos.size(); ++i) {
        // Every dealable combo meets at least one runout on every street.
        const double equity = tally.doubled_wins[i] / (2.0 * tally.runouts[i]);
        result.equity[combos[i].index] = static_cast<float>(equity);
        result.valid.set(combos[i].index);
    }
    return result;
}

std::bitset<kComboCount> combos_in(const WeightedRange& range) {
    std::bitset<kComboCount> combos;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (range.weight(combo) > 0.0f) combos.set(static_cast<std::size_t>(combo));
    }
    return combos;
}

std::optional<double> equity_vs_range(const ComboEquities& equities, const WeightedRange& range) {
    double weighted = 0.0;
    double mass = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        const double weight = range.weight(combo);
        if (!(weight > 0.0) || (combo_mask(combo) & equities.dead_cards) != 0) continue;
        if (!equities.valid.test(static_cast<std::size_t>(combo))) {
            throw std::invalid_argument("equity_vs_range: the range holds a combo that was not computed");
        }
        weighted += weight * equities.equity[static_cast<std::size_t>(combo)];
        mass += weight;
    }
    if (!(mass > 0.0)) return std::nullopt;
    return weighted / mass;
}

MultiwayRangeEquity equity_vs_ranges(const HoleCards& hero, std::span<const Card> board,
                                     std::span<const std::optional<WeightedRange>, kMaxSeats> ranges,
                                     std::span<const LayerRequest> layers, const RangeEquityOptions& options) {
    const CardMask dead = validate_cards(hero, board, options.threads, "equity_vs_ranges");
    if (options.multiway_rollouts <= 0) {
        throw std::invalid_argument("equity_vs_ranges: multiway_rollouts must be positive");
    }
    if (layers.empty()) throw std::invalid_argument("equity_vs_ranges: no layers requested");

    MultiwayScenario scenario;
    scenario.hero = hero;
    std::copy(board.begin(), board.end(), scenario.board.begin());
    scenario.board_size = static_cast<int>(board.size());
    std::vector<Card> known(hero.begin(), hero.end());
    known.insert(known.end(), board.begin(), board.end());
    scenario.deck = Deck(known);

    std::array<int, kMaxSeats> table_of{};
    table_of.fill(-1);
    for (int s = 0; s < kMaxSeats; ++s) {
        const std::optional<WeightedRange>& range = ranges[static_cast<std::size_t>(s)];
        if (!range) continue;
        VillainTable table;
        double running = 0.0;
        for (int combo = 0; combo < kComboCount; ++combo) {
            const double weight = range->weight(combo);
            if (!(weight > 0.0) || (combo_mask(combo) & dead) != 0) continue;
            running += weight;
            table.combos.push_back(static_cast<std::uint16_t>(combo));
            table.cumulative.push_back(running);
        }
        if (table.combos.empty()) {
            throw std::invalid_argument("equity_vs_ranges: a range has no weight left beside hero's and the board's cards");
        }
        table_of[static_cast<std::size_t>(s)] = static_cast<int>(scenario.villains.size());
        scenario.villains.push_back(std::move(table));
    }
    // Hero holds one seat. Six villains would also make a tie seven ways, which
    // 60ths cannot split evenly.
    if (scenario.villains.size() > static_cast<std::size_t>(kMaxSeats - 1)) {
        throw std::invalid_argument("equity_vs_ranges: more villain ranges than seats beside hero's");
    }

    for (const LayerRequest& layer : layers) {
        if (layer.villain_seats == 0 || (layer.villain_seats >> kMaxSeats) != 0) {
            throw std::invalid_argument("equity_vs_ranges: a layer needs villains, by seat index below kMaxSeats");
        }
        std::vector<int> members;
        for (int s = 0; s < kMaxSeats; ++s) {
            if ((layer.villain_seats & (1u << s)) == 0) continue;
            if (table_of[static_cast<std::size_t>(s)] < 0) {
                throw std::invalid_argument("equity_vs_ranges: a layer names a seat index with no range");
            }
            members.push_back(table_of[static_cast<std::size_t>(s)]);
        }
        scenario.layer_villains.push_back(std::move(members));
    }

    const int rollouts = options.multiway_rollouts;
    const int chunks = (rollouts - 1) / kRangeChunkRollouts + 1;
    std::vector<MultiwayChunk> results(static_cast<std::size_t>(chunks));
    const int automatic = rollouts < kParallelMultiwayRollouts ? 1 : hardware_threads();
    run_parallel(chunks, options.threads == 0 ? automatic : options.threads, [&](int chunk, int) {
        const int size = chunk == chunks - 1 ? rollouts - chunk * kRangeChunkRollouts : kRangeChunkRollouts;
        results[static_cast<std::size_t>(chunk)] =
            run_multiway_chunk(scenario, mix(options.seed, static_cast<std::uint64_t>(chunk)), size);
    });

    MultiwayRangeEquity result;
    result.rollouts = rollouts;
    std::vector<LayerTally> total(layers.size());
    for (const MultiwayChunk& chunk : results) {
        if (chunk.gave_up) {
            throw std::invalid_argument("equity_vs_ranges: the ranges can almost never be dealt together");
        }
        result.restarts += chunk.restarts;
        for (std::size_t layer = 0; layer < total.size(); ++layer) {
            total[layer].share += chunk.layers[layer].share;
            total[layer].share_squared += chunk.layers[layer].share_squared;
        }
    }

    const double n = static_cast<double>(rollouts);
    const double units = static_cast<double>(kShareUnits);
    for (const LayerTally& tally : total) {
        const double mean = static_cast<double>(tally.share) / units / n;
        const double mean_square = static_cast<double>(tally.share_squared) / (units * units) / n;
        double error = 0.0;
        if (rollouts > 1) error = std::sqrt(std::max(0.0, mean_square - mean * mean) * n / (n - 1.0) / n);
        result.equity.push_back(mean);
        result.std_error.push_back(error);
    }
    return result;
}

}  // namespace poker
