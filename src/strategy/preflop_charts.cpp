#include "strategy/preflop_charts.h"

#include <stdexcept>

namespace poker {
namespace {

constexpr std::string_view kRankChars = "23456789TJQKA";

// Playability-aware order, not raw equity versus a random hand, which would put
// weak offsuit aces above good suited connectors.
constexpr std::string_view kStrengthOrder =
    "AA KK QQ JJ AKs TT AKo AQs AJs KQs 99 ATs AQo KJs 88 KTs QJs AJo KQo QTs A9s 77 ATo JTs KJo "
    "A8s K9s QJo A7s KTo Q9s A5s 66 A6s QTo J9s A9o T9s A4s K8s JTo K7s A8o A3s Q8s K9o A2s K6s "
    "J8s T8s A7o 55 Q9o 98s K5s Q7s J9o A5o T9o A6o K4s K8o Q6s J7s T7s A4o 97s K3s 87s Q5s K7o "
    "44 Q8o A3o K2s J8o Q4s T8o J6s K6o A2o T6s 98o 76s 86s 96s Q3s J5s K5o Q7o Q2s J4s 33 65s "
    "J7o T7o K4o 75s T5s Q6o J3s 95s 87o 85s 97o T4s K3o J2s 54s Q5o 64s T3s 22 K2o 74s 76o T2s "
    "Q4o J6o 84s 94s 86o T6o 96o 53s 93s Q3o J5o 63s 43s 92s 73s 65o Q2o J4o 83s 75o 52s 85o 82s "
    "T5o 95o J3o 62s 54o 42s T4o J2o 72s 64o T3o 32s 74o 84o T2o 94o 53o 93o 63o 43o 92o 73o 83o "
    "52o 82o 42o 62o 72o 32o";

struct StrengthTable {
    std::array<HandClass, kHandClassCount> order{};
    std::array<int, kHandClassCount> rank_of{};
    std::array<double, kHandClassCount> percentile_of{};

    StrengthTable() {
        std::bitset<kHandClassCount> seen;
        std::size_t count = 0;
        int cumulative = 0;

        std::string_view rest = kStrengthOrder;
        while (!rest.empty()) {
            const std::size_t space = rest.find(' ');
            const auto hand = HandClass::parse(rest.substr(0, space));
            if (!hand || count == order.size() || seen.test(static_cast<std::size_t>(hand->index()))) {
                throw std::logic_error("preflop_charts: bad strength order");
            }
            const auto index = static_cast<std::size_t>(hand->index());
            seen.set(index);
            cumulative += hand->combos();
            order[count] = *hand;
            rank_of[index] = static_cast<int>(count);
            percentile_of[index] = 100.0 * cumulative / kTotalCombos;
            ++count;
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        }
        if (count != order.size()) throw std::logic_error("preflop_charts: incomplete strength order");
    }
};

const StrengthTable& strength_table() {
    static const StrengthTable table;
    return table;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

HandClass parse_hand_or_throw(std::string_view text) {
    const auto hand = HandClass::parse(text);
    if (!hand) throw std::invalid_argument("HandRange: bad hand '" + std::string(text) + "'");
    return *hand;
}

Rank rank_at(int value) { return static_cast<Rank>(value); }

void add_token(HandRange& range, std::string_view token) {
    const auto dash = token.find('-');
    if (dash != std::string_view::npos) {
        const HandClass a = parse_hand_or_throw(token.substr(0, dash));
        const HandClass b = parse_hand_or_throw(token.substr(dash + 1));
        if (a.is_pair() != b.is_pair() || a.suited != b.suited || (!a.is_pair() && a.high != b.high)) {
            throw std::invalid_argument("HandRange: bad span '" + std::string(token) + "'");
        }
        const int low = static_cast<int>(a.low < b.low ? a.low : b.low);
        const int high = static_cast<int>(a.low < b.low ? b.low : a.low);
        for (int r = low; r <= high; ++r) {
            range.add(a.is_pair() ? HandClass::of(rank_at(r), rank_at(r), false)
                                  : HandClass::of(a.high, rank_at(r), a.suited));
        }
        return;
    }

    if (token.ends_with('+')) {
        const HandClass base = parse_hand_or_throw(token.substr(0, token.size() - 1));
        if (base.is_pair()) {
            for (int r = static_cast<int>(base.low); r <= static_cast<int>(Rank::Ace); ++r) {
                range.add(HandClass::of(rank_at(r), rank_at(r), false));
            }
        } else {
            for (int r = static_cast<int>(base.low); r < static_cast<int>(base.high); ++r) {
                range.add(HandClass::of(base.high, rank_at(r), base.suited));
            }
        }
        return;
    }

    range.add(parse_hand_or_throw(token));
}

constexpr std::size_t kPositionCount = 6;

struct Charts {
    std::array<HandRange, kPositionCount> rfi;
    HandRange heads_up_open;

    VsRaiseRanges in_position_vs_early;
    VsRaiseRanges in_position_vs_late;
    VsRaiseRanges sb_vs_early;
    VsRaiseRanges sb_vs_cutoff;
    VsRaiseRanges sb_vs_button;
    VsRaiseRanges bb_vs_early;
    VsRaiseRanges bb_vs_cutoff;
    VsRaiseRanges bb_vs_button;
    VsRaiseRanges bb_vs_sb;
    VsRaiseRanges bb_heads_up;

    VsRaiseRanges vs_three_bet;
    VsRaiseRanges vs_four_bet;

    static VsRaiseRanges ranges(std::string_view raise, std::string_view call) {
        return {HandRange::parse(raise), HandRange::parse(call)};
    }

    Charts() {
        const auto rfi_at = [this](Position position) -> HandRange& {
            return rfi[static_cast<std::size_t>(position)];
        };
        // About 16%, 20%, 28%, 46% and 43% of combos. The BB is left empty.
        rfi_at(Position::UTG) = HandRange::parse("66+, A2s+, K9s+, QTs+, JTs, T9s, 98s, ATo+, KJo+");
        rfi_at(Position::HJ) =
            HandRange::parse("44+, A2s+, K8s+, Q9s+, J9s+, T8s+, 98s, 87s, 76s, ATo+, KTo+, QJo");
        rfi_at(Position::CO) = HandRange::parse(
            "22+, A2s+, K5s+, Q8s+, J8s+, T8s+, 97s+, 86s+, 75s+, 65s, 54s, A8o+, KTo+, QTo+, JTo");
        rfi_at(Position::BTN) = HandRange::parse(
            "22+, A2s+, K2s+, Q2s+, J4s+, T6s+, 96s+, 85s+, 74s+, 64s+, 53s+, 43s, A2o+, K8o+, Q9o+, J9o+, T8o+, "
            "98o");
        rfi_at(Position::SB) = HandRange::parse(
            "22+, A2s+, K2s+, Q3s+, J5s+, T6s+, 96s+, 85s+, 75s+, 64s+, 54s, A2o+, K8o+, Q9o+, J9o+, T9o");
        // About 75%: heads-up the button is in position with a discount on the blind.
        heads_up_open = HandRange::parse(
            "22+, A2s+, K2s+, Q2s+, J2s+, T2s+, 92s+, 82s+, 73s+, 63s+, 52s+, 42s+, 32s, A2o+, K2o+, Q2o+, J4o+, "
            "T6o+, 96o+, 86o+, 76o, 65o");

        in_position_vs_early =
            ranges("QQ+, AKs, AKo, A5s, A4s", "JJ-66, AQs-ATs, KQs, KJs, QJs, JTs, T9s, 98s, AQo");
        in_position_vs_late = ranges("JJ+, AQs+, AKo, AQo, A5s-A3s, KJs+",
                                     "TT-44, AJs-A8s, KTs, QTs+, J9s+, T9s, 98s, 87s, AJo, KQo");

        // Out of position the SB mostly plays 3-bet or fold.
        sb_vs_early = ranges("JJ+, AQs+, AKo, A5s, A4s, KQs", "");
        sb_vs_cutoff = ranges("TT+, ATs+, AQo+, A5s-A2s, KTs+, QJs", "");
        sb_vs_button = ranges("88+, A8s+, A5s-A2s, KTs+, QTs+, JTs, T9s, ATo+, KJo+", "");

        // The BB closes the action at a discount, so defends widest.
        bb_vs_early = ranges(
            "QQ+, AKs, AKo, A5s",
            "JJ-22, AQs-A6s, A4s-A2s, K9s+, Q9s+, J9s+, T8s+, 98s, 87s, 76s, 65s, 54s, AQo-ATo, KQo, KJo");
        bb_vs_cutoff = ranges("JJ+, AQs+, AKo, A5s-A3s, KQs",
                              "TT-22, AJs-A6s, A2s, K6s+, Q8s+, J8s+, T7s+, 97s+, 86s+, 75s+, 64s+, 54s, "
                              "AQo-A8o, KTo+, QTo+, JTo");
        bb_vs_button = ranges("TT+, AJs+, A5s-A2s, AQo+, KQs",
                              "99-22, ATs-A6s, K2s+, Q5s+, J6s+, T6s+, 96s+, 85s+, 74s+, 63s+, 53s+, 43s, "
                              "AJo-A4o, K8o+, Q9o+, J9o+, T8o+, 98o, 87o");
        bb_vs_sb = ranges("99+, ATs+, A5s-A2s, AJo+, KJs+",
                          "88-22, A9s-A6s, K2s+, Q2s+, J4s+, T6s+, 95s+, 85s+, 74s+, 63s+, 53s+, 43s, ATo-A2o, K7o+, "
                          "Q8o+, J8o+, T8o+, 97o+, 87o, 76o");
        bb_heads_up = ranges("99+, ATs+, A5s-A2s, AJo+, KJs+",
                             "88-22, A9s-A6s, K2s+, Q2s+, J2s+, T3s+, 93s+, 84s+, 74s+, 63s+, 52s+, 42s+, 32s, "
                             "ATo-A2o, K2o+, Q5o+, J7o+, T7o+, 97o+, 86o+, 76o, 65o");

        // A5s stays as the 4-bet bluff: it blocks aces and plays badly as a call.
        vs_three_bet = ranges("QQ+, AKs, AKo, A5s", "JJ-88, AQs, AJs, ATs, KQs, KJs, QJs, JTs, T9s, AQo");
        vs_four_bet = ranges("KK+, AKs", "QQ, JJ, AKo");
    }
};

const Charts& charts() {
    static const Charts instance;
    return instance;
}

bool is_early(Position position) { return position == Position::UTG || position == Position::HJ; }

PreflopResponse respond(const VsRaiseRanges& ranges, HandClass hand) {
    if (ranges.raise.contains(hand)) return PreflopResponse::Raise;
    if (ranges.call.contains(hand)) return PreflopResponse::Call;
    return PreflopResponse::Fold;
}

}  // namespace

std::optional<HandClass> HandClass::parse(std::string_view text) {
    if (text.size() != 2 && text.size() != 3) return std::nullopt;

    const auto a = kRankChars.find(text[0]);
    const auto b = kRankChars.find(text[1]);
    if (a == std::string_view::npos || b == std::string_view::npos) return std::nullopt;

    const bool pair = a == b;
    if (pair != (text.size() == 2)) return std::nullopt;
    if (!pair && text[2] != 's' && text[2] != 'o') return std::nullopt;

    return of(static_cast<Rank>(a), static_cast<Rank>(b), !pair && text[2] == 's');
}

std::string HandClass::to_string() const {
    std::string text{kRankChars[static_cast<std::size_t>(high)], kRankChars[static_cast<std::size_t>(low)]};
    if (!is_pair()) text += suited ? 's' : 'o';
    return text;
}

const std::array<HandClass, kHandClassCount>& hand_classes_by_strength() { return strength_table().order; }

int strength_rank(HandClass hand) { return strength_table().rank_of[static_cast<std::size_t>(hand.index())]; }

double strength_percentile(HandClass hand) {
    return strength_table().percentile_of[static_cast<std::size_t>(hand.index())];
}

HandRange HandRange::parse(std::string_view text) {
    HandRange range;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::string_view token = trim(text.substr(0, comma));
        // Only a wholly blank string is the empty range; a stray comma is a typo.
        if (token.empty() && (comma != std::string_view::npos || !range.empty())) {
            throw std::invalid_argument("HandRange: empty token");
        }
        if (!token.empty()) add_token(range, token);
        if (comma == std::string_view::npos) return range;
        text = text.substr(comma + 1);
    }
}

HandRange HandRange::top_percent(double share) {
    HandRange range;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (strength_percentile(hand) > share) break;
        range.add(hand);
    }
    return range;
}

int HandRange::combos() const {
    int total = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (contains(hand)) total += hand.combos();
    }
    return total;
}

const HandRange& rfi_range(Position position, int players) {
    const Charts& c = charts();
    if (players == 2 && position == Position::BTN) return c.heads_up_open;
    return c.rfi[static_cast<std::size_t>(position)];
}

bool should_open(HandClass hand, Position position, int players) {
    return rfi_range(position, players).contains(hand);
}

const VsRaiseRanges& vs_open_ranges(Position hero, Position opener, int players) {
    const Charts& c = charts();
    if (hero == Position::BB) {
        if (players == 2) return c.bb_heads_up;
        if (is_early(opener)) return c.bb_vs_early;
        if (opener == Position::CO) return c.bb_vs_cutoff;
        return opener == Position::BTN ? c.bb_vs_button : c.bb_vs_sb;
    }
    if (hero == Position::SB) {
        if (is_early(opener)) return c.sb_vs_early;
        return opener == Position::CO ? c.sb_vs_cutoff : c.sb_vs_button;
    }
    return is_early(opener) ? c.in_position_vs_early : c.in_position_vs_late;
}

PreflopResponse vs_open_response(HandClass hand, Position hero, Position opener, int players) {
    return respond(vs_open_ranges(hero, opener, players), hand);
}

PreflopResponse vs_three_bet_response(HandClass hand) { return respond(charts().vs_three_bet, hand); }

PreflopResponse vs_four_bet_response(HandClass hand) { return respond(charts().vs_four_bet, hand); }

PreflopTier preflop_tier(HandClass hand) {
    const double percentile = strength_percentile(hand);
    if (percentile <= 3.5) return PreflopTier::Premium;
    if (percentile <= 12.0) return PreflopTier::Strong;
    if (percentile <= 30.0) return PreflopTier::Playable;
    if (percentile <= 55.0) return PreflopTier::Speculative;
    return PreflopTier::Trash;
}

}  // namespace poker
