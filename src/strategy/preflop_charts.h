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

// Acting order after the flop, 0 first: SB, BB, UTG, HJ, CO, BTN. Heads-up the
// button (which posts the small blind) acts last, as here.
int postflop_acting_order(Position position);

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

// The advisor's and range model's answers to a single raise, at 100 BB 6-max with
// an open to about 2.5 BB: wider than the bots' vs_open_ranges, in the style of
// published solver outputs, and with 3-bets that mix value and a polarized share
// (suited wheel aces, suited connectors and broadways). The BB defends about 32%
// against UTG, 38% HJ, 49% CO, 58% BTN and 66% SB (3-bets 8.1, 9.5, 11.5, 13.4 and
// 16.3%), and 72% heads-up (16.9%). The SB plays 3-bet or fold (6.5% against UTG
// up to 14.2% against the BTN), so its call range is empty. A non-blind hero cold
// 3-bets 5-10% and flats 3-9%, more on the button and against later opens. Each
// 3-bet range and each defended range contains the one against an earlier opener.
// Order is not enforced, as for vs_open_ranges: a non-blind hero facing a later
// or blind opener uses the BTN-versus-CO table. The bots never use these.
const VsRaiseRanges& advisor_vs_open_ranges(Position hero, Position opener, int players = kMaxSeats);

// Facing a 3-bet or a 4-bet. These are not positional: their ranges are so narrow
// that position moves only the marginal hands. The bots use these; the advisor's
// positional tables are below, and the two must not be merged.
PreflopResponse vs_three_bet_response(HandClass hand);
PreflopResponse vs_four_bet_response(HandClass hand);

// The opener's answer to a 3-bet, by position, at 100 BB 6-max with an open to
// 2.5 BB and a 3-bet to about 3x in position or 3.5-4x from the blinds: `raise`
// is the 4-bet, `call` the flat. Every hand in either set is in the opener's own
// RFI range. Out of position (a 3-bet from a later non-blind position) the opener
// continues about 35-43% of its open, in position against a blind 3-bet about
// 45-50%, and earlier openers keep a larger share of their tighter ranges.
// Heads-up (players == 2) the button answers the big blind from the heads-up open.
// Order is not enforced: an SB or BB "opener" (a raise over limps) uses the
// blind-versus-blind table, and a BTN opener always counts as in position.
const VsRaiseRanges& vs_three_bet_ranges(Position opener, Position three_bettor, int players = kMaxSeats);

// The 3-bettor's answer to a 4-bet: `raise` is the 5-bet all-in, `call` the flat.
// In position (a non-blind 3-bettor) it calls more and jams less than a blind
// 3-bettor out of position; a 4-bet from UTG or HJ is answered tighter, and
// blind-versus-blind and heads-up 4-bets wider. Position is by postflop order, so
// a cold 4-bet uses the same tables.
const VsRaiseRanges& vs_four_bet_ranges(Position three_bettor, Position four_bettor, int players = kMaxSeats);

enum class PreflopTier : std::uint8_t { Premium, Strong, Playable, Speculative, Trash };

constexpr std::string_view to_string(PreflopTier tier) {
    constexpr std::array<std::string_view, 5> names{"premium", "strong", "playable", "speculative", "trash"};
    return names[static_cast<std::size_t>(tier)];
}

// Tier by strength percentile, for rationale text.
PreflopTier preflop_tier(HandClass hand);

// `chart` narrowed or widened to about `width_mult` times its combos, keeping its
// shape: below 1 it drops the chart's weakest classes, above 1 it adds the
// strongest classes outside it, both in hand_classes_by_strength() order. For the
// range model's player types; the bots never call it.
//
// The target is chart.combos() * width_mult, rounded to the nearest class
// boundary: walking the classes in that order, a class is kept while the combos
// before it plus half of its own are within the target. It is also a hard cap:
// no class is kept that would take the result past cap_pct percent of all 1326
// combos. So the result is the identity at width_mult 1 whenever the chart fits
// the cap, is empty once the target is below half the chart's strongest class,
// and, for a fixed chart and cap, grows by nesting as width_mult grows. An empty
// chart stays empty. A negative or NaN width_mult counts as 0; cap_pct is
// clamped to [0, 100].
HandRange resize_range(const HandRange& chart, double width_mult, double cap_pct = 100.0);

}  // namespace poker
