#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>

#include "engine/game_types.h"
#include "engine/player_view.h"
#include "engine/pots.h"

namespace poker {

struct GameConfig {
    int starting_stack = 2000;
    int small_blind = 10;
    int big_blind = 20;
    int ante = 0;
    AnteType ante_type = AnteType::None;
    std::uint64_t seed = 0;

    // The session ends when this seat busts. Without a hero, play continues
    // until one player has all the chips.
    std::optional<int> hero_seat = 0;

    // Per-seat stacks overriding starting_stack; a seat with 0 starts busted.
    std::optional<std::array<int, kMaxSeats>> stacks;

    // First hand's button; randomly chosen among live seats if unset.
    std::optional<int> first_button;
};

// Fixed cards for one hand, in place of a shuffled deck. Every live seat needs
// hole cards, and all cards must be distinct.
struct ScriptedDeal {
    std::array<std::optional<std::array<Card, 2>>, kMaxSeats> holes{};
    std::array<Card, 5> board{};
};

// Headless No-Limit Hold'em freezeout state machine.
//
//   BetweenHands --start_hand--> Betting --(hand ends)--> BetweenHands or SessionOver
//   any phase    --new_game----> BetweenHands
//
// Rules worth knowing:
// - The button moves to the next seat with chips each hand (no dead button).
// - Heads-up, the button posts the small blind, acts first preflop and last after.
// - A short big blind still sets the amount to call at the full big blind; any
//   excess a caller puts in comes back as an uncalled refund.
// - An all-in below a full raise does not reopen betting for players who have
//   already acted, unless short all-ins add up to a full raise (TDA Rule 47).
// - Once at most one player can still act, remaining streets deal out automatically.
// - Blinds and ante come from the level set for the hand (set_level between hands).
//   Antes are dead money outside total_bet: per-player antes are posted before the
//   blinds and layer into side pots like any chips; a big-blind ante is posted after
//   the blind, from what it leaves, and goes to the main pot. Neither changes the
//   bet to call, the minimum raise or the uncalled refund.
class GameEngine {
public:
    // Throws std::invalid_argument for an unusable config.
    explicit GameEngine(GameConfig config);

    // Resets stacks and seats for a fresh freezeout. The RNG keeps advancing.
    void new_game();

    // The same with a new config (stacks, blinds, ante, hero, first button). The
    // seed is ignored: the RNG keeps advancing, so a new game never replays the
    // deals of the first. Throws std::invalid_argument for an unusable config, and
    // then changes nothing.
    void new_game(GameConfig config);

    // Blinds and ante from the next hand on. Throws std::logic_error unless the
    // phase is BetweenHands, and std::invalid_argument for an unusable level.
    void set_level(const BlindLevel& level);
    const BlindLevel& next_level() const { return level_; }

    // Moves the button, deals and posts blinds. Throws std::logic_error unless
    // the phase is BetweenHands, and std::invalid_argument for a bad script.
    void start_hand(const std::optional<ScriptedDeal>& script = std::nullopt);

    const GameState& state() const { return state_; }

    // Legal options for the player to act; an empty mask if nobody is to act.
    LegalActions legal_actions() const;

    // Sanitized snapshot for one seat; the only thing to hand a bot or the advisor.
    // Throws std::invalid_argument for a seat out of range.
    PlayerView player_view(int seat) const;

    // Main and side pots as they stand now, for display during a hand.
    LivePots live_pots() const;

    // Applies an action for the player to act. Returns false and changes
    // nothing if the action is illegal.
    [[nodiscard]] bool apply_action(const Action& action);

    SessionSummary session_summary() const;

private:
    int next_live_seat(int from) const;
    bool needs_action(int seat) const;
    int count_with_status(SeatStatus status) const;
    bool betting_round_complete() const;
    void commit_chips(int seat, int street_total);
    void post_ante(int seat, int amount);
    std::array<int, kMaxSeats> pot_contributions() const;
    int main_pot_dead_money() const;  // A big-blind ante; per-player antes layer instead
    void settle(int from_seat);
    void deal_street_cards();
    void finish_hand(bool showdown);

    GameConfig config_;
    BlindLevel level_;  // For the next hand
    std::mt19937_64 rng_;
    GameState state_;
    std::array<Card, 5> planned_board_{};
};

}  // namespace poker
