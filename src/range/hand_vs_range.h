#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/card.h"
#include "engine/table_types.h"
#include "equity/equity.h"
#include "range/combos.h"
#include "range/weighted_range.h"

namespace poker {

// Flop runouts per turn card at or above which the flop is enumerated exactly
// (half of the 46 other cards: see hero_vs_each_combo).
inline constexpr int kExactFlopRiversPerTurn = 23;

// Multiway rollouts per independently seeded chunk: as in equity.cpp, chunks own
// the random streams, so a result depends only on the seed and the rollout count.
inline constexpr int kRangeChunkRollouts = 1024;

struct RangeEquityOptions {
    std::uint64_t seed = 0;
    // Flop only: river cards sampled per turn card. 0, or kExactFlopRiversPerTurn
    // and above, enumerates every runout. Exact is the default because it fits the
    // budget (about 22 ms on one thread); a sample of 8 is about 3x faster but
    // misses single combos by up to 6 pp, so keep it for cheap test loops.
    int flop_rivers_per_turn = 0;
    int threads = 0;  // 0 = automatic; otherwise use up to this many threads
    int multiway_rollouts = 3000;
};

// Hero's heads-up equity (wins plus half of ties, in [0, 1]) against each villain
// combo, by combo_index.
struct ComboEquities {
    std::array<float, kComboCount> equity{};
    // Set for each combo that was computed: requested, and sharing no card with
    // hero or the board. Every other entry of `equity` is 0.
    std::bitset<kComboCount> valid;
    // Hero's cards and the board: combos holding one cannot be dealt.
    CardMask dead_cards = 0;
};

// Hero's equity against each combo in `needed`, over the rest of the board:
// - River and turn: exact, every river card.
// - Flop: a balanced sample. The 47 unseen cards are put in an order shuffled
//   from `seed`; the turn at position i gets the K river cards at positions
//   i+1 ... i+K (mod 47), K = flop_rivers_per_turn. So every card is the turn
//   once and the river K times, which balances both streets' main effects (a
//   flush card, a pairing card) rather than leaving them to chance. A combo
//   skips the turns it holds, and past a river it holds, takes the next
//   position, so each of its turns gets exactly K rivers. K = 23 uses each of
//   the 1081 runouts exactly once, which is the exact enumeration.
// - Preflop: class_vs_class_equity of hero's class against the combo's, which
//   averages over suits and so ignores hero's exact cards blocking the combo's.
// The results are bit-identical for any thread count.
// Throws std::invalid_argument for duplicate or invalid cards, a board of other
// than 0, 3, 4 or 5 cards, or negative threads or flop_rivers_per_turn.
ComboEquities hero_vs_each_combo(const HoleCards& hero, std::span<const Card> board,
                                 const std::bitset<kComboCount>& needed, const RangeEquityOptions& options = {});

// The combos with positive weight.
std::bitset<kComboCount> combos_in(const WeightedRange& range);

// Hero's equity against `range`: the weighted mean over its combos with positive
// weight, skipping those that hold a dead card. nullopt when no weight is left.
// Throws std::invalid_argument when a combo with positive weight that could be
// dealt was not computed, since leaving it out would silently skew the result.
std::optional<double> equity_vs_range(const ComboEquities& equities, const WeightedRange& range);

// One pot to score: hero against the villains whose bit is set (bit i = seat
// index i, as in PotLayer::opponent_seats).
struct LayerRequest {
    std::uint8_t villain_seats = 0;
};

struct MultiwayRangeEquity {
    std::vector<double> equity;     // Per layer: hero's mean pot share, in [0, 1]
    std::vector<double> std_error;  // Per layer: standard error of `equity`
    int rollouts = 0;
    // Deals thrown away because two villains' sampled combos shared a card.
    std::uint64_t restarts = 0;
};

// Hero against one weighted range per seat index (nullopt = not in the hand), by
// Monte Carlo. Each rollout deals every villain a combo from its range, with
// probability proportional to weight, plus the rest of the board; then each layer
// scores hero's share among hero and that layer's villains, ties split evenly.
// Villains outside a layer still hold their cards, which the board cannot use.
// A deal in which two villains share a card is redrawn whole, which samples the
// joint distribution exactly (a villain-by-villain redraw would not: it ignores
// how much each villain's hand blocks the next one's range).
// Deterministic for a given seed and rollout count, whatever the thread count.
// Throws std::invalid_argument for duplicate or invalid cards, a bad board size,
// non-positive rollouts or negative threads, more than kMaxSeats - 1 ranges, no
// layers, a layer with no villain or naming a seat index past kMaxSeats or one
// with no range, a range with no weight left once hero's and the board's cards
// are removed, and ranges that can almost never be dealt together.
MultiwayRangeEquity equity_vs_ranges(const HoleCards& hero, std::span<const Card> board,
                                     std::span<const std::optional<WeightedRange>, kMaxSeats> ranges,
                                     std::span<const LayerRequest> layers, const RangeEquityOptions& options = {});

}  // namespace poker
