#include "bots/gto_heuristic_engine.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace poker {
namespace {

// Straight windows run over 14 positions: 0 is the low ace, 13 the high ace.
unsigned straight_positions(Card card) {
    const int rank = static_cast<int>(card.rank());
    unsigned mask = 1u << (rank + 1);
    if (card.rank() == Rank::Ace) mask |= 1u;
    return mask;
}

constexpr int kStraightWindows = 10;

unsigned window_mask(int start) { return 0x1Fu << start; }

Action check_or_fold() { return {ActionType::Fold, 0}; }  // legalize makes a free fold a check

int count_preflop(const PlayerView& view, ActionType type) {
    return static_cast<int>(std::count_if(view.actions.begin(), view.actions.end(), [type](const ActionRecord& a) {
        return a.street == Street::Preflop && a.type == type;
    }));
}

int players_dealt_in(const PlayerView& view) {
    return static_cast<int>(std::count_if(view.seats.begin(), view.seats.end(),
                                          [](const PublicSeat& s) { return s.position.has_value(); }));
}

bool can_raise(const PlayerView& view) {
    return view.legal.allows(ActionType::Bet) || view.legal.allows(ActionType::Raise);
}

// A call that risks most of the chips actually in play needs a stronger range,
// and a covering stack does not change that: only what the opponent can match is
// at risk. Below 10 big blinds the price is what matters, so the usual tables
// still apply; from 50 the call is a huge one however deep everybody is.
bool call_is_committing(const PlayerView& view) {
    constexpr int kAlwaysPriceBigBlinds = 10;
    constexpr int kAlwaysCommitBigBlinds = 50;

    const int call = view.legal.call_amount;
    if (call <= view.big_blind * kAlwaysPriceBigBlinds) return false;
    if (call >= view.big_blind * kAlwaysCommitBigBlinds) return true;

    const PublicSeat& me = view.seats[view.seat];
    int deepest_opponent = 0;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const PublicSeat& s = view.seats[seat];
        if (seat == view.seat || (s.status != SeatStatus::Active && s.status != SeatStatus::AllIn)) continue;
        deepest_opponent = std::max(deepest_opponent, s.street_bet + s.stack);
    }
    return call * 3 >= std::min(me.stack, deepest_opponent - me.street_bet);
}

// The wheel aces in the charts' raise ranges are there to block aces, not to play
// for value: they have no call to fall back on when the raise is not available.
bool is_chart_bluff_raise(HandClass hand) {
    return hand.suited && hand.high == Rank::Ace && hand.low <= Rank::Five;
}

// The latest bet or raise this street, in chips added, and the pot before it.
struct Aggression {
    int chips = 0;
    int pot_before = 0;
};

std::optional<Aggression> last_aggression(const PlayerView& view) {
    int added_since = 0;
    for (auto it = view.actions.rbegin(); it != view.actions.rend() && it->street == view.street; ++it) {
        added_since += it->chips_added;
        if (it->type == ActionType::Bet || it->type == ActionType::Raise) {
            return Aggression{it->chips_added, view.total_pot - added_since};
        }
    }
    return std::nullopt;
}

// A raise taking this much of the stack leaves nothing to fold with, so go all in.
constexpr double kPreflopShoveShare = 0.40;

Action preflop_raise(const PlayerView& view, int to) {
    const PublicSeat& me = view.seats[view.seat];
    const int total = me.street_bet + me.stack;
    if (to >= kPreflopShoveShare * total) to = total;
    return {ActionType::Raise, to};
}

// By default 2.5 big blinds plus one per limper, 3x against a raise plus one bet per
// cold caller, 2.3x against a re-raise, then all in. Chip counts are far below 2^53,
// so the whole-number multiples give exactly the integer products they replaced.
int preflop_size(const PlayerView& view, int raises, const PreflopSizing& sizing) {
    const int callers = count_preflop(view, ActionType::Call);
    switch (raises) {
        case 0:
            return static_cast<int>(std::lround(view.big_blind * (sizing.open_bb + callers)));
        case 1:
            return static_cast<int>(std::lround(view.current_bet * (sizing.vs_raise_mult + callers)));
        case 2:
            return static_cast<int>(std::lround(view.current_bet * sizing.vs_reraise_mult));
        default:
            return view.seats[view.seat].street_bet + view.seats[view.seat].stack;
    }
}

Position first_raiser_position(const PlayerView& view) {
    for (const ActionRecord& a : view.actions) {
        if (a.street == Street::Preflop && a.type == ActionType::Raise) {
            return view.seats[a.seat].position.value_or(Position::BTN);
        }
    }
    return Position::BTN;
}

// A uniform draw in [0, 1) from the raw generator bits, so it is the same on every
// standard library. Only the later knobs use it, and only when they are on.
double unit_roll(std::mt19937_64& rng) { return static_cast<double>(rng() >> 11) * 0x1.0p-53; }

// True when this player made the last bet or raise of the street before this one.
bool led_previous_street(const PlayerView& view) {
    if (view.street == Street::Preflop) return false;
    const Street previous = static_cast<Street>(static_cast<int>(view.street) - 1);
    for (auto it = view.actions.rbegin(); it != view.actions.rend(); ++it) {
        if (it->street != previous) continue;
        // The record holds the resolved type: an all-in shows as the bet, raise or call it was.
        if (it->type == ActionType::Bet || it->type == ActionType::Raise) return it->seat == view.seat;
    }
    return false;
}

double value_bar(int opponents, double strength) {
    const double even_share = 1.0 / (opponents + 1);
    return even_share + (1.0 - even_share) * strength;
}

Action aggressive(const PlayerView& view, double fraction) {
    // Leaving a sliver behind only tells the opponent what to do; bet it all.
    constexpr double kShoveIfLeftBehindUnder = 0.25;

    const PublicSeat& me = view.seats[view.seat];
    int to = raise_to_for_pot_fraction(view, fraction);
    const int total = me.street_bet + me.stack;
    const int after_bet_pot = view.total_pot + (to - me.street_bet) * 2;
    if (total - to <= kShoveIfLeftBehindUnder * after_bet_pot) to = total;
    return {view.legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise, to};
}

}  // namespace

BoardTexture classify_board(std::span<const Card> board) {
    BoardTexture texture;
    if (board.empty()) return texture;

    std::array<int, kRankCount> rank_counts{};
    std::array<int, kSuitCount> suit_counts{};
    unsigned positions = 0;
    for (const Card card : board) {
        ++rank_counts[static_cast<std::size_t>(card.rank())];
        ++suit_counts[static_cast<std::size_t>(card.suit())];
        positions |= straight_positions(card);
    }
    texture.paired = std::any_of(rank_counts.begin(), rank_counts.end(), [](int n) { return n >= 2; });

    const int max_suit = *std::max_element(suit_counts.begin(), suit_counts.end());
    texture.monotone = board.size() >= 3 && max_suit == static_cast<int>(board.size());

    int straight_cards = 0;
    for (int start = 0; start < kStraightWindows; ++start) {
        straight_cards = std::max(straight_cards, std::popcount(positions & window_mask(start)));
    }

    // Two of a suit is a flush draw for anyone holding two more, so long as a card
    // is still to come. Three in a straight window means a straight is possible,
    // the low ace included: A-5-2 really does make one with a 3-4.
    const bool flush_draw_live = max_suit == 2 && board.size() < 5;
    texture.wet = max_suit >= 3 || flush_draw_live || straight_cards >= 3;

    bool connected = false;
    for (int start = 0; start + 3 <= 14; ++start) connected = connected || ((positions >> start) & 7u) == 7u;
    texture.dynamic = board.size() < 5 && ((flush_draw_live && straight_cards >= 3) || connected);
    return texture;
}

std::string_view texture_name(const BoardTexture& texture, bool has_board) {
    if (!has_board) return "no board";
    if (texture.monotone) return "monotone";
    if (texture.paired) return "paired";
    return texture.wet ? "wet" : "dry";
}

HandRead read_hand(const std::array<Card, 2>& hole, std::span<const Card> board) {
    HandRead read;
    read.holds_pair = hole[0].rank() == hole[1].rank();
    if (board.empty()) return read;

    std::vector<Card> cards{hole[0], hole[1]};
    cards.insert(cards.end(), board.begin(), board.end());
    read.rank = evaluate(cards);
    read.category = hand_category(read.rank);

    for (const Card card : board) {
        if (card.rank() == hole[0].rank() || card.rank() == hole[1].rank()) read.holds_pair = true;
    }
    if (board.size() >= 5) return read;  // No cards to come: no draws

    std::array<int, kSuitCount> suit_counts{};
    for (const Card card : cards) ++suit_counts[static_cast<std::size_t>(card.suit())];
    for (const Card card : hole) {
        if (suit_counts[static_cast<std::size_t>(card.suit())] == 4) read.flush_draw = true;
    }

    unsigned all_positions = 0;
    unsigned board_positions = 0;
    for (const Card card : cards) all_positions |= straight_positions(card);
    for (const Card card : board) board_positions |= straight_positions(card);
    // A hole card repeating a board rank adds nothing: everyone has that draw.
    const unsigned hole_positions = (straight_positions(hole[0]) | straight_positions(hole[1])) & ~board_positions;

    unsigned completing_ranks = 0;  // Bit per Rank, so the two ace positions count once
    for (int start = 0; start < kStraightWindows; ++start) {
        const unsigned window = window_mask(start);
        if (std::popcount(all_positions & window) != 4 || (hole_positions & window) == 0) continue;
        const int missing = std::countr_zero(window & ~all_positions);
        completing_ranks |= 1u << (missing == 0 ? static_cast<int>(Rank::Ace) : missing - 1);
    }
    const int outs_ranks = std::popcount(completing_ranks);
    read.open_ended = outs_ranks >= 2;
    read.gutshot = outs_ranks == 1;
    return read;
}

bool is_nutted(const HandRead& read, std::span<const Card> board) {
    if (!read.category || *read.category > HandCategory::ThreeOfAKind) return false;
    if (*read.category == HandCategory::ThreeOfAKind && !read.holds_pair) return false;  // Trips on the board
    if (board.size() == 5) {
        const std::array<Card, 5> only_board{board[0], board[1], board[2], board[3], board[4]};
        if (evaluate5(only_board) == read.rank) return false;
    }
    // Before the river only one hand can play the board: four of a kind on the turn.
    if (board.size() == 4 && *read.category == HandCategory::FourOfAKind &&
        std::all_of(board.begin(), board.end(), [&](Card c) { return c.rank() == board[0].rank(); })) {
        return false;
    }
    return true;
}

PotMath pot_math(const PlayerView& view) {
    PotMath math;
    const PublicSeat& me = view.seats[view.seat];
    math.pot = view.total_pot;
    math.facing_bet = std::max(0, view.current_bet - me.street_bet);
    math.to_call = std::min(math.facing_bet, me.stack);

    const int my_total_after_call = me.total_bet + math.to_call;
    for (const PublicSeat& s : view.seats) math.contestable += std::min(s.total_bet, my_total_after_call);
    math.contestable += view.dead_money();  // Antes: in every pot this seat can win

    if (math.to_call > 0) {
        math.pot_odds = static_cast<double>(math.to_call) / (math.contestable + math.to_call);
    }
    if (math.facing_bet > 0) {
        // Blinds alone are not a bet anybody chose, so there is no MDF to report.
        if (const auto bet = last_aggression(view)) {
            // Chips the stack cannot match are not part of the bet this seat faces.
            const int effective = std::max(0, bet->chips - (math.facing_bet - math.to_call));
            if (effective + bet->pot_before > 0) {
                math.facing_aggression = true;
                math.mdf = static_cast<double>(bet->pot_before) / (bet->pot_before + effective);
                if (bet->pot_before > 0) math.bet_fraction = static_cast<double>(effective) / bet->pot_before;
            }
        }
    }

    int deepest_opponent = 0;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (seat != view.seat && view.seats[seat].status == SeatStatus::Active) {
            deepest_opponent = std::max(deepest_opponent, view.seats[seat].stack);
        }
    }
    const int pot_after_call = math.pot + math.to_call;
    if (pot_after_call > 0) {
        math.spr = static_cast<double>(std::min(me.stack - math.to_call, deepest_opponent)) / pot_after_call;
    }
    return math;
}

std::vector<PotLayer> pot_layers(const PlayerView& view) {
    const PublicSeat& me = view.seats[view.seat];
    const int my_total = me.total_bet + std::min(std::max(0, view.current_bet - me.street_bet), me.stack);

    std::vector<int> levels{my_total};
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const PublicSeat& s = view.seats[seat];
        if (seat != view.seat && s.status == SeatStatus::AllIn && s.total_bet < my_total) levels.push_back(s.total_bet);
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    std::vector<PotLayer> layers;
    int previous = 0;
    for (const int level : levels) {
        PotLayer layer;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            const PublicSeat& s = view.seats[seat];
            const int total = seat == view.seat ? my_total : s.total_bet;
            layer.amount += std::min(total, level) - std::min(total, previous);
            if (seat == view.seat) continue;
            // Someone still active can match any level; an all-in is capped at its own.
            const auto bit = static_cast<std::uint8_t>(1u << seat);
            if (s.status == SeatStatus::Active) {
                ++layer.opponents;
                layer.opponent_seats = static_cast<std::uint8_t>(layer.opponent_seats | bit);
            }
            if (s.status == SeatStatus::AllIn && s.total_bet >= level) {
                ++layer.opponents;
                ++layer.all_in_opponents;
                layer.opponent_seats = static_cast<std::uint8_t>(layer.opponent_seats | bit);
            }
        }
        if (layer.amount > 0) layers.push_back(layer);
        previous = level;
    }
    // Antes are dead money in the main pot. (A player all in from the ante alone
    // contests only the antes; that small side layer is not modelled.)
    if (!layers.empty()) layers.front().amount += view.dead_money();
    return layers;
}

double EquityTable::against(int opponents) {
    if (opponents <= 0) return 1.0;  // Dead money with nobody left to contest it
    const auto index = static_cast<std::size_t>(std::min(opponents, kMaxOpponents));
    if (!known_[index]) {
        EquityOptions options = options_;
        options.seed += index * 0x9E3779B97F4A7C15ULL;  // Separate streams, not the same runouts reused
        known_[index] = equity_vs_random(hole_, board_, static_cast<int>(index), options).equity;
    }
    return *known_[index];
}

double price_equity(std::span<const PotLayer> layers, EquityTable& equities) {
    double weighted = 0.0;
    int total = 0;
    for (const PotLayer& layer : layers) {
        weighted += layer.amount * equities.against(layer.opponents);
        total += layer.amount;
    }
    return total > 0 ? weighted / total : 0.0;
}

int raise_to_for_pot_fraction(const PlayerView& view, double fraction) {
    const int facing = std::max(0, view.current_bet - view.seats[view.seat].street_bet);
    const double pot_after_call = view.total_pot + facing;
    return view.current_bet + static_cast<int>(std::lround(fraction * pot_after_call));
}

int clamp_raise_to(const PlayerView& view, int to) {
    // Chips nobody can match come straight back, so never bet more than the
    // deepest opponent can put in. Players already all in have no decision left.
    int matchable = view.current_bet;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const PublicSeat& s = view.seats[seat];
        if (seat != view.seat && s.status == SeatStatus::Active) matchable = std::max(matchable, s.street_bet + s.stack);
    }
    return std::clamp(std::min(to, matchable), view.legal.min_raise_to, view.legal.max_raise_to);
}

double bet_fraction_for(const BoardTexture& texture, bool polarized) {
    if (polarized) return kPolarizedBetFraction;
    return texture.wet ? kWetBetFraction : kDryBetFraction;
}

Action legalize(const PlayerView& view, Action action) {
    const LegalActions& legal = view.legal;
    const Action passive = legal.allows(ActionType::Check) ? Action{ActionType::Check, 0}
                           : legal.allows(ActionType::Call) ? Action{ActionType::Call, 0}
                                                            : Action{ActionType::Fold, 0};
    switch (action.type) {
        case ActionType::Fold:
            return legal.allows(ActionType::Check) ? Action{ActionType::Check, 0} : Action{ActionType::Fold, 0};
        case ActionType::Check:
            return legal.allows(ActionType::Check) ? Action{ActionType::Check, 0} : Action{ActionType::Fold, 0};
        case ActionType::Call:
            return passive;
        case ActionType::Bet:
        case ActionType::Raise:
            if (legal.allows(ActionType::Bet)) return {ActionType::Bet, clamp_raise_to(view, action.amount)};
            if (legal.allows(ActionType::Raise)) return {ActionType::Raise, clamp_raise_to(view, action.amount)};
            return passive;
        case ActionType::AllIn:
            return legal.allows(ActionType::AllIn) ? Action{ActionType::AllIn, 0} : passive;
    }
    return passive;
}

PersonalityProfile nit_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Nit";
    p.preflop_style = PreflopStyle::TopPercent;
    p.cutoffs = {.play = kNitPlayPercent,
                 .open_raise = kNitPlayPercent,
                 .three_bet = 3.5,
                 .call_raise = kNitPlayPercent,
                 .four_bet = 2.0,
                 .call_three_bet = 3.5,
                 .commit = 3.5};
    p.call_margin = kNitCallMargin;
    p.bluff_freq = kNitBluffFreq;
    p.folds_non_nuts_to_large_bets = true;
    p.value_bet_strength = 0.45;
    p.value_raise_strength = 0.70;
    p.polarized_strength = 0.85;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile loose_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Loose";
    p.preflop_style = PreflopStyle::TopPercent;
    p.cutoffs = {.play = kLoosePlayPercent,
                 .open_raise = 20.0,
                 .three_bet = 8.0,
                 .call_raise = kLoosePlayPercent,
                 .four_bet = 3.5,
                 .call_three_bet = 25.0,
                 .commit = 15.0};
    p.call_margin = kLooseCallMargin;
    p.bluff_freq = kLooseBluffFreq;
    p.bluff_raise_share = 0.25;
    p.calls_any_pair_or_draw = true;
    p.value_bet_strength = 0.20;
    p.value_raise_strength = 0.45;
    p.polarized_strength = 0.80;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile gto_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "GTO";
    p.preflop_style = PreflopStyle::PositionalCharts;
    p.balanced = true;
    p.bet_pressure_discount = kGtoBetPressureDiscount;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile tag_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "TAG";
    p.cutoffs = {.play = 20.0, .open_raise = 20.0, .three_bet = 6.0, .call_raise = 12.0, .four_bet = 2.5,
                 .call_three_bet = 6.0, .commit = 4.0};
    p.positional_width = 0.6;
    p.call_margin = 0.05;
    p.bluff_freq = 0.12;
    p.bluff_raise_share = 0.15;
    p.bet_pressure_discount = 0.05;
    p.value_bet_strength = 0.30;
    p.value_raise_strength = 0.55;
    p.polarized_strength = 0.82;
    p.bet_sizes = {.dry = 0.5, .wet = 0.75, .polarized = 1.25};
    p.cbet_freq = 0.65;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile lag_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "LAG";
    p.cutoffs = {.play = 32.0, .open_raise = 32.0, .three_bet = 12.0, .call_raise = 20.0, .four_bet = 4.5,
                 .call_three_bet = 12.0, .commit = 8.0};
    p.positional_width = 0.8;
    p.call_margin = -0.02;
    p.bluff_freq = 0.25;
    p.bluff_raise_share = 0.35;
    p.bet_pressure_discount = 0.05;
    p.value_bet_strength = 0.20;
    p.value_raise_strength = 0.45;
    p.polarized_strength = 0.78;
    p.bet_sizes = {.dry = 0.5, .wet = 0.8, .polarized = 1.5};
    p.cbet_freq = 0.75;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile station_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Station";
    p.cutoffs = {.play = 60.0, .open_raise = 8.0, .three_bet = 3.0, .call_raise = 60.0, .four_bet = 2.0,
                 .call_three_bet = 35.0, .commit = 20.0};
    p.call_margin = -0.20;
    p.bluff_freq = 0.05;
    p.calls_any_pair_or_draw = true;
    p.value_bet_strength = 0.35;
    p.value_raise_strength = 0.75;
    p.polarized_strength = 0.90;
    p.bet_sizes = {.dry = 0.33, .wet = 0.5, .polarized = 0.75};
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile maniac_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Maniac";
    p.cutoffs = {.play = 70.0, .open_raise = 70.0, .three_bet = 25.0, .call_raise = 45.0, .four_bet = 12.0,
                 .call_three_bet = 40.0, .commit = 25.0};
    p.positional_width = 0.4;
    p.preflop_sizing = {.open_bb = 4.0, .vs_raise_mult = 4.0, .vs_reraise_mult = 2.5};
    p.call_margin = -0.08;
    p.bluff_freq = 0.55;
    p.bluff_raise_share = 0.45;
    p.calls_any_pair_or_draw = true;
    p.value_bet_strength = 0.15;
    p.value_raise_strength = 0.35;
    p.polarized_strength = 0.70;
    p.bet_sizes = {.dry = 0.66, .wet = 1.0, .polarized = 1.5};
    p.cbet_freq = 0.9;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile rock_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Rock";
    p.cutoffs = {.play = 8.0, .open_raise = 8.0, .three_bet = 2.0, .call_raise = 5.0, .four_bet = 1.5,
                 .call_three_bet = 2.5, .commit = 2.0};
    p.positional_width = 0.3;
    p.call_margin = 0.10;
    p.value_bet_strength = 0.50;
    p.value_raise_strength = 0.80;
    p.polarized_strength = 0.90;
    p.bet_sizes = {.dry = 0.5, .wet = 0.66, .polarized = 1.0};
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile trapper_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Trapper";
    p.cutoffs = {.play = 18.0, .open_raise = 15.0, .three_bet = 1.5, .call_raise = 18.0, .four_bet = 1.5,
                 .call_three_bet = 6.0, .commit = 5.0};
    p.positional_width = 0.3;
    p.bluff_freq = 0.06;
    p.value_bet_strength = 0.30;
    p.value_raise_strength = 0.50;
    p.polarized_strength = 0.75;
    p.slowplay_freq = 0.6;
    p.rollouts = rollouts;
    return p;
}

PersonalityProfile bully_profile(int rollouts) {
    PersonalityProfile p;
    p.name = "Bully";
    p.cutoffs = {.play = 28.0, .open_raise = 28.0, .three_bet = 10.0, .call_raise = 14.0, .four_bet = 4.0,
                 .call_three_bet = 8.0, .commit = 6.0};
    p.positional_width = 0.5;
    p.preflop_sizing = {.open_bb = 3.5, .vs_raise_mult = 4.0, .vs_reraise_mult = 2.6};
    p.call_margin = 0.05;
    p.bluff_freq = 0.20;
    p.bluff_raise_share = 0.20;
    p.bet_pressure_discount = 0.05;
    p.value_bet_strength = 0.30;
    p.value_raise_strength = 0.55;
    p.polarized_strength = 0.80;
    p.bet_sizes = {.dry = 0.75, .wet = 1.25, .polarized = 2.0};
    p.cbet_freq = 0.8;
    p.rollouts = rollouts;
    return p;
}

PreflopCutoffs cutoffs_at(const PersonalityProfile& profile, std::optional<Position> position) {
    PreflopCutoffs cut = profile.cutoffs;
    if (profile.positional_width == 0.0 || !position) return cut;
    const double weight = kPositionalWidthWeights[static_cast<std::size_t>(*position)];
    const double scale = std::max(0.2, 1.0 + profile.positional_width * weight);
    for (double* share : {&cut.play, &cut.open_raise, &cut.three_bet, &cut.call_raise}) {
        *share = std::min(100.0, *share * scale);
    }
    return cut;
}

double GtoHeuristicEngine::bet_size(const BoardTexture& texture, bool polarized) const {
    if (polarized) return profile_.bet_sizes.polarized;
    return texture.wet ? profile_.bet_sizes.wet : profile_.bet_sizes.dry;
}

Action GtoHeuristicEngine::decide(const PlayerView& view, std::mt19937_64& rng) const {
    if (!view.own_hole || view.legal.mask == 0) return {ActionType::Fold, 0};

    Action action;
    if (view.street != Street::Preflop) {
        action = postflop(view, rng);
    } else if (profile_.preflop_style == PreflopStyle::PositionalCharts) {
        action = preflop_by_chart(view);
    } else {
        action = preflop_by_percent(view);
    }
    return legalize(view, action);
}

Action GtoHeuristicEngine::preflop_by_percent(const PlayerView& view) const {
    const HandClass hand = HandClass::from_cards((*view.own_hole)[0], (*view.own_hole)[1]);
    const double percentile = strength_percentile(hand);
    const PreflopCutoffs cut = cutoffs_at(profile_, view.position);
    const int raises = count_preflop(view, ActionType::Raise);
    const Action call{ActionType::Call, 0};
    const Action raise = preflop_raise(view, preflop_size(view, raises, profile_.preflop_sizing));

    if (raises == 0) {
        if (percentile <= cut.open_raise) return raise;
        if (view.legal.allows(ActionType::Check)) return {ActionType::Check, 0};
        return percentile <= cut.play ? call : check_or_fold();
    }
    if (raises >= 3 || call_is_committing(view)) {
        if (percentile > cut.commit) return check_or_fold();
        return percentile <= cut.four_bet ? raise : call;
    }
    if (raises == 1) {
        if (percentile <= cut.three_bet) return raise;
        return percentile <= cut.call_raise ? call : check_or_fold();
    }
    if (percentile <= cut.four_bet) return raise;
    return percentile <= cut.call_three_bet ? call : check_or_fold();
}

PreflopResponse chart_response(const PlayerView& view) {
    const HandClass hand = HandClass::from_cards((*view.own_hole)[0], (*view.own_hole)[1]);
    const Position position = view.position.value_or(Position::BTN);
    const int players = players_dealt_in(view);
    const int raises = count_preflop(view, ActionType::Raise);

    PreflopResponse response = PreflopResponse::Fold;
    if (raises == 0) {
        // Limped to the big blind: raise for value, check the rest for free.
        const bool raises_first_in = position == Position::BB ? preflop_tier(hand) <= PreflopTier::Strong
                                                              : should_open(hand, position, players);
        response = raises_first_in ? PreflopResponse::Raise : PreflopResponse::Fold;
    } else if (raises >= 3 || call_is_committing(view)) {
        response = vs_four_bet_response(hand);
    } else if (raises == 1) {
        response = vs_open_response(hand, position, first_raiser_position(view), players);
    } else {
        response = vs_three_bet_response(hand);
    }

    if (response == PreflopResponse::Raise && !can_raise(view) && is_chart_bluff_raise(hand)) {
        response = PreflopResponse::Fold;
    }
    // The ranges assume a normal price. At a tiny one no range is worth folding.
    if (response == PreflopResponse::Fold) {
        const PotMath math = pot_math(view);
        const double even_share = 1.0 / (view.opponents_in_hand() + 1);
        if (math.to_call > 0 && math.pot_odds <= kCheapCallEvenShare * even_share) response = PreflopResponse::Call;
    }
    return response;
}

Action GtoHeuristicEngine::preflop_by_chart(const PlayerView& view) const {
    switch (chart_response(view)) {
        case PreflopResponse::Raise:
            return preflop_raise(view,
                                 preflop_size(view, count_preflop(view, ActionType::Raise), profile_.preflop_sizing));
        case PreflopResponse::Call: return {ActionType::Call, 0};
        case PreflopResponse::Fold: break;
    }
    return check_or_fold();
}

double GtoHeuristicEngine::bluff_probability(const HandRead& read, Street street, const BoardTexture& texture,
                                             int opponents) const {
    if (!profile_.balanced) return profile_.bluff_freq;
    if (opponents > 2) return 0.0;  // Everyone has to fold: bluffs stop working multiway

    // Bluffs that make a bet of size s break even for the caller: s / (1 + 2s) of
    // the betting range. Draws fill that share first because they can still win
    // when called; on the river, with no draws left, air fills half of it.
    const double s = bet_size(texture, street == Street::River);
    const double balanced_share = s / (1.0 + 2.0 * s);
    if (street == Street::River) return 0.5 * balanced_share;
    if (read.strong_draw()) return std::min(1.0, 2.0 * balanced_share);
    if (read.gutshot) return balanced_share;
    return 0.25 * balanced_share;
}

Action GtoHeuristicEngine::postflop(const PlayerView& view, std::mt19937_64& rng) const {
    const std::array<Card, 2>& hole = *view.own_hole;
    const BoardTexture texture = classify_board(view.board);
    const HandRead read = read_hand(hole, view.board);
    const PotMath math = pot_math(view);
    const int opponents = std::clamp(view.opponents_in_hand(), 1, kMaxOpponents);

    EquityOptions options;
    options.rollouts = profile_.rollouts;
    options.seed = rng();
    EquityTable equities(hole, view.board, options);
    const double equity = equities.against(opponents);

    std::uniform_real_distribution<double> roll(0.0, 1.0);
    const bool river = view.street == Street::River;
    const bool very_strong = equity >= value_bar(opponents, profile_.polarized_strength);
    const bool bluff_candidate = !read.holds_pair || read.has_draw();

    // Later knobs draw only when they are on, so the classic profiles' streams stay as they were.
    const bool slowplay = very_strong && !river && profile_.slowplay_freq > 0.0;

    if (math.to_call == 0) {
        if (equity >= value_bar(opponents, profile_.value_bet_strength)) {
            if (slowplay && unit_roll(rng) < profile_.slowplay_freq) return {ActionType::Check, 0};
            return aggressive(view, bet_size(texture, river && very_strong));
        }
        if (profile_.cbet_freq > 0.0 && opponents <= 2 && led_previous_street(view) &&
            unit_roll(rng) < profile_.cbet_freq) {
            return aggressive(view, bet_size(texture, river));
        }
        if (bluff_candidate && roll(rng) < bluff_probability(read, view.street, texture, opponents)) {
            return aggressive(view, bet_size(texture, river));
        }
        return {ActionType::Check, 0};
    }

    const bool large_bet = math.bet_fraction >= kLargeBetFraction;
    const bool nutted = is_nutted(read, view.board);
    if (profile_.folds_non_nuts_to_large_bets && large_bet && !nutted) return check_or_fold();

    // A value raise is what the street bars are for; Nits also keep theirs to real hands.
    const bool can_value_raise = equity >= value_bar(opponents, profile_.value_raise_strength) &&
                                 (!profile_.folds_non_nuts_to_large_bets || nutted);
    if (can_value_raise) {
        if (slowplay && unit_roll(rng) < profile_.slowplay_freq) return {ActionType::Call, 0};
        return aggressive(view, bet_size(texture, river && very_strong));
    }

    const double raise_bluff = profile_.balanced ? (read.strong_draw() && opponents <= 2 ? 0.15 : 0.0)
                                                 : profile_.bluff_freq * profile_.bluff_raise_share;
    // Where no raise is legal a bluff has nothing to do: it must not turn into a call.
    if (raise_bluff > 0.0 && can_raise(view) && bluff_candidate && roll(rng) < raise_bluff) {
        return aggressive(view, bet_size(texture, river));
    }

    // Measured layer by layer, as a short all-in cannot win the side pot.
    const std::vector<PotLayer> layers = pot_layers(view);
    const double adjusted = price_equity(layers, equities) - bet_pressure(math, profile_.bet_pressure_discount);
    if (adjusted >= math.pot_odds + profile_.call_margin) return {ActionType::Call, 0};

    if (profile_.calls_any_pair_or_draw && (read.holds_pair || read.has_draw())) return {ActionType::Call, 0};

    if (profile_.balanced) {
        // Folding everything below the price would over-fold. Marginal hands with
        // something to show defend at MDF, which is what keeps the bettor's bluffs
        // from printing money.
        constexpr double kBluffCatchBand = 0.08;
        if ((read.holds_pair || read.has_draw()) && adjusted >= math.pot_odds - kBluffCatchBand &&
            roll(rng) < math.mdf) {
            return {ActionType::Call, 0};
        }
    }
    return check_or_fold();
}

}  // namespace poker
