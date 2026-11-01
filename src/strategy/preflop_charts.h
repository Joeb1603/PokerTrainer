#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/card.h"
#include "engine/table_types.h"

namespace poker {

inline constexpr int kHandClassCount = 169;
inline constexpr int kTotalCombos = 1326;

// One of the 169 starting hands, such as AKs, AKo or TT. `high` >= `low`, and a
// pair is never suited; build through of() or from_cards() to keep that true.
struct HandClass {
    Rank high = Rank::Ace;
    Rank low = Rank::Ace;
    bool suited = false;

    constexpr auto operator<=>(const HandClass&) const = default;

    static constexpr HandClass of(Rank a, Rank b, bool is_suited) {
        return HandClass{a > b ? a : b, a > b ? b : a, a != b && is_suited};
    }
    static constexpr HandClass from_cards(Card a, Card b) { return of(a.rank(), b.rank(), a.suit() == b.suit()); }

    // Parses "AKs", "AKo" or "TT". Nullopt for anything else, including "AK" and "TTs".
    static std::optional<HandClass> parse(std::string_view text);

    std::string to_string() const;

    constexpr bool is_pair() const { return high == low; }
    constexpr int combos() const { return is_pair() ? 6 : suited ? 4 : 12; }

    // Unique id in [0, 168]: the usual 13x13 grid read row by row, AA first, with
    // suited hands above the diagonal and offsuit below it.
    constexpr int index() const {
        const int row = 12 - static_cast<int>(high);
        const int col = 12 - static_cast<int>(low);
        return suited || is_pair() ? row * kRankCount + col : col * kRankCount + row;
    }
};

// All 169 classes from strongest to weakest.
const std::array<HandClass, kHandClassCount>& hand_classes_by_strength();

// 0 for AA up to 168 for the weakest hand.
int strength_rank(HandClass hand);

// Percent of all combos at least as strong as `hand`, in (0, 100]: AA is 0.45.
double strength_percentile(HandClass hand);

// A set of hand classes.
class HandRange {
public:
    // Comma-separated, e.g. "66+, A2s+, KTs, JJ-88, AQo-ATo, 97s+". "+" raises the
    // low card up to one below the high card (or, for a pair, up to AA); a dash
    // spans two hands with the same high card. Throws std::invalid_argument.
    static HandRange parse(std::string_view text);

    // The strongest classes whose cumulative combos stay within `share` percent.
    static HandRange top_percent(double share);

    bool contains(HandClass hand) const { return members_.test(static_cast<std::size_t>(hand.index())); }
    void add(HandClass hand) { members_.set(static_cast<std::size_t>(hand.index())); }
    bool contains_all(const HandRange& other) const { return (other.members_ & ~members_).none(); }
    bool empty() const { return members_.none(); }

    int combos() const;
    double percent() const { return 100.0 * combos() / kTotalCombos; }

private:
    std::bitset<kHandClassCount> members_;
};

// Opening ranges when folded to this position. BB has no open, so its range is
// empty. `players` is the number dealt in: only heads-up (2) changes the range,
// since a short table just drops the early positions.
const HandRange& rfi_range(Position position, int players = kMaxSeats);
bool should_open(HandClass hand, Position position, int players = kMaxSeats);

enum class PreflopResponse : std::uint8_t { Fold, Call, Raise };

constexpr std::string_view to_string(PreflopResponse response) {
    constexpr std::array<std::string_view, 3> names{"Fold", "Call", "Raise"};
    return names[static_cast<std::size_t>(response)];
}

struct VsRaiseRanges {
    HandRange raise;  // Takes precedence over `call`
    HandRange call;
};

// Response ranges for `hero` facing a single raise from `opener`. Order is not
// enforced: a hero who limped and got raised uses the same table.
const VsRaiseRanges& vs_open_ranges(Position hero, Position opener, int players = kMaxSeats);
PreflopResponse vs_open_response(HandClass hand, Position hero, Position opener, int players = kMaxSeats);

// Facing a 3-bet or a 4-bet. These are not positional: their ranges are so narrow
// that position moves only the marginal hands.
PreflopResponse vs_three_bet_response(HandClass hand);
PreflopResponse vs_four_bet_response(HandClass hand);

enum class PreflopTier : std::uint8_t { Premium, Strong, Playable, Speculative, Trash };

constexpr std::string_view to_string(PreflopTier tier) {
    constexpr std::array<std::string_view, 5> names{"premium", "strong", "playable", "speculative", "trash"};
    return names[static_cast<std::size_t>(tier)];
}

// Tier by strength percentile, for rationale text.
PreflopTier preflop_tier(HandClass hand);

}  // namespace poker
