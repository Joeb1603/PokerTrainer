#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "core/card.h"
#include "core/hand_evaluator.h"
#include "engine/table_types.h"
#include "engine/player_view.h"
#include "equity/equity.h"
#include "strategy/preflop_charts.h"

namespace poker {

// --- Board and hand reading --------------------------------------------------

struct BoardTexture {
    bool paired = false;
    bool monotone = false;  // Three or more cards, all one suit
    bool wet = false;       // Two of a suit with cards to come, three of a suit, or three in a straight window
    // With a card to come: a flush draw and three in a straight window, or three connected ranks.
    bool dynamic = false;

    bool dry() const { return !wet; }
};

// Preflop (no cards) reads as dry.
BoardTexture classify_board(std::span<const Card> board);

// "no board", "monotone", "paired", "wet" or "dry"; the most specific one wins.
std::string_view texture_name(const BoardTexture& texture, bool has_board);

struct HandRead {
    std::optional<HandCategory> category;  // Nullopt before the flop
    HandRank rank = kWorstHandRank;
    bool holds_pair = false;  // A pocket pair or a hole card pairing the board; a paired board alone is not one
    bool flush_draw = false;
    bool open_ended = false;  // Two ranks complete a straight, including double gutshots
    bool gutshot = false;

    bool has_draw() const { return flush_draw || open_ended || gutshot; }
    bool strong_draw() const { return flush_draw || open_ended; }
};

// Draws are reported only with cards still to come, and need a hole card.
HandRead read_hand(const std::array<Card, 2>& hole, std::span<const Card> board);

// Trips or better that the hole cards actually make. Not hands playing the board.
bool is_nutted(const HandRead& read, std::span<const Card> board);

// --- Pot arithmetic ----------------------------------------------------------

struct PotMath {
    int to_call = 0;       // Chips a call adds, capped at the stack
    int facing_bet = 0;    // Chips needed to match the bet, not capped
    int pot = 0;           // Everything in the middle now
    int contestable = 0;   // The part of `pot` a call can win: excess over a short call comes back
    double pot_odds = 0.0;
    bool facing_aggression = false;  // A bet or raise this street, not just the blinds
    // Bet is the last aggressor's chips added, cut to what this stack can match, and
    // Pot is the pot before them. MDF = Pot / (Pot + Bet), or 1 with no aggression.
    double mdf = 1.0;
    double bet_fraction = 0.0;  // Bet / Pot, by the same definitions
    double spr = 0.0;           // Effective stack over the pot, both measured after calling
};

PotMath pot_math(const PlayerView& view);

// A slice of the chips a call plays for, with the opponents who can win it. Excess
// the stack cannot match is left out, so the amounts total contestable + to_call.
struct PotLayer {
    int amount = 0;
    int opponents = 0;
    int all_in_opponents = 0;  // Of `opponents`: these cannot be made to fold
    std::uint8_t opponent_seats = 0;  // Bit i set when seat index i contests this layer
};

std::vector<PotLayer> pot_layers(const PlayerView& view);

// Equity by number of opponents, rolled out on first use. A side pot asks for a
// second count only when a short all-in is in the hand.
class EquityTable {
public:
    EquityTable(const std::array<Card, 2>& hole, std::span<const Card> board, const EquityOptions& options)
        : hole_(hole), board_(board), options_(options) {}

    // 1 for nobody left to beat. `board` must outlive the table.
    double against(int opponents);

private:
    std::array<Card, 2> hole_;
    std::span<const Card> board_;
    EquityOptions options_;
    std::array<std::optional<double>, kMaxOpponents + 1> known_{};
};

// The equity to compare with pot odds: each layer's equity, weighted by its chips.
double price_equity(std::span<const PotLayer> layers, EquityTable& equities);

// Random-hand equity flatters a hand against a real betting range, so it loses
// this per pot of bet size, up to 1.5 pots, before being compared to a price.
inline constexpr double kGtoBetPressureDiscount = 0.10;

constexpr double bet_pressure(const PotMath& math, double discount) {
    return discount * (math.bet_fraction < 1.5 ? math.bet_fraction : 1.5);
}

// A chart fold becomes a call at or below this share of an even split of the pot:
// 20% pot odds heads-up, 10% against three. Even the worst hand holds at least half
// of an even split against random hands, so this is clear of a losing call.
inline constexpr double kCheapCallEvenShare = 0.40;

// The street total for a bet or raise of `fraction` of the pot, measured after
// calling. Not clamped. A pot of 200 with no bet and 0.75 gives 150.
int raise_to_for_pot_fraction(const PlayerView& view, double fraction);

// `to` clamped to what the opponents can match and to the legal bounds.
int clamp_raise_to(const PlayerView& view, int to);

inline constexpr double kDryBetFraction = 0.33;
inline constexpr double kWetBetFraction = 0.75;
inline constexpr double kPolarizedBetFraction = 1.25;

double bet_fraction_for(const BoardTexture& texture, bool polarized);

// The nearest legal action to `action`: a free fold becomes a check, a raise that
// cannot be made becomes a call. Bet and Raise swap to whichever exists, and sizes
// are clamped. Every decision passes through here.
Action legalize(const PlayerView& view, Action action);

// What the positional charts say for this preflop spot. Fold means "fold or take
// the free check"; first in, Raise means open. Raise can still come back where no
// raise is legal, meaning "call": except for the bluffs, which come back as Fold.
PreflopResponse chart_response(const PlayerView& view);

// --- Personalities -----------------------------------------------------------

enum class PreflopStyle : std::uint8_t { TopPercent, PositionalCharts };

// Percent-of-combos cut-offs for TopPercent preflop play.
struct PreflopCutoffs {
    double play = 100.0;            // Enter the pot at all; the headline range
    double open_raise = 100.0;      // First in, raises rather than limps
    double three_bet = 0.0;         // Facing one raise
    double call_raise = 0.0;
    double four_bet = 0.0;          // Facing a re-raise
    double call_three_bet = 0.0;
    double commit = 0.0;            // Facing a bet taking a big share of the stack
};

struct PersonalityProfile {
    std::string_view name;
    PreflopStyle preflop_style = PreflopStyle::TopPercent;
    PreflopCutoffs cutoffs;

    double call_margin = 0.0;  // Calls when equity >= pot odds + margin
    double bluff_freq = 0.0;   // With a hand that has no pair, when checked to
    double bluff_raise_share = 0.0;  // Bluff raises when facing a bet, as a share of bluff_freq

    bool calls_any_pair_or_draw = false;
    bool folds_non_nuts_to_large_bets = false;
    // Balanced bluffs from draw equity instead of bluff_freq, plus MDF defence.
    bool balanced = false;

    double bet_pressure_discount = 0.0;  // See kGtoBetPressureDiscount

    // Equity bars as a position between an even share of the pot (0) and a
    // certain win (1), so they relax naturally as opponents are added.
    double value_bet_strength = 0.25;
    double value_raise_strength = 0.50;
    double polarized_strength = 0.80;

    int rollouts = kDefaultRollouts;
};

inline constexpr double kNitCallMargin = 0.15;
inline constexpr double kNitBluffFreq = 0.03;
inline constexpr double kNitPlayPercent = 12.0;
inline constexpr double kLooseCallMargin = -0.12;
inline constexpr double kLooseBluffFreq = 0.35;
inline constexpr double kLoosePlayPercent = 55.0;
inline constexpr double kLargeBetFraction = 0.75;  // Of the pot before the bet

PersonalityProfile nit_profile(int rollouts = kDefaultRollouts);
PersonalityProfile loose_profile(int rollouts = kDefaultRollouts);
PersonalityProfile gto_profile(int rollouts = kDefaultRollouts);

// Shared decision logic. It holds no RNG or per-hand state, so one bot's choices
// depend only on its view and the generator it passes in.
class GtoHeuristicEngine {
public:
    explicit GtoHeuristicEngine(PersonalityProfile profile) : profile_(profile) {}

    // Always legal for `view.legal`. Folds if the seat holds no cards or is not to act.
    Action decide(const PlayerView& view, std::mt19937_64& rng) const;

    const PersonalityProfile& profile() const { return profile_; }

private:
    Action preflop_by_percent(const PlayerView& view) const;
    Action preflop_by_chart(const PlayerView& view) const;
    Action postflop(const PlayerView& view, std::mt19937_64& rng) const;
    double bluff_probability(const HandRead& read, Street street, const BoardTexture& texture,
                             int opponents) const;

    PersonalityProfile profile_;
};

}  // namespace poker
