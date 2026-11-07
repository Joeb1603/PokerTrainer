#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "advisor/training_advisor.h"
#include "controller/solver_service.h"
#include "core/card.h"
#include "engine/pots.h"
#include "engine/table_types.h"
#include "logging/hand_logger.h"

namespace poker {

inline constexpr int kHeroSeat = 0;

enum class ControllerPhase { BotToAct, HeroToAct, HandComplete, SessionOver };

// A seat as Hero may see it. Opponent cards appear only once shown at showdown, and
// the name is always the public one: Bot 5 is "Shifter", never its active profile.
struct SeatSnapshot {
    std::string name;
    std::optional<Position> position;  // Nullopt if not dealt in
    int stack = 0;
    int street_bet = 0;
    SeatStatus status = SeatStatus::Active;
    bool is_button = false;
    bool is_to_act = false;
    bool holds_cards = false;  // Dealt in and has not folded
    std::optional<std::array<Card, 2>> visible_hole;
    std::optional<std::string> shown_hand;  // Hand name, once shown at showdown
    int won = 0;                            // Chips won from pots in the finished hand
    bool revealed_after_fold = false;       // visible_hole is a folded bot's, shown on request
};

struct PayoutLine {
    std::string name;
    int amount = 0;
    std::optional<std::string> hand;  // Absent if everyone else folded
};

struct SessionStats {
    std::string session_id;
    int hands_played = 0;
    int hero_net = 0;
    bool hero_busted = false;
    std::optional<std::string> winner;
    int mistakes = 0;
    double ev_lost = 0.0;
};

// Solver mode, as the front end shows it. The advice is for Hero's own cards only,
// from the public table and the range model, so it holds no hidden information.
struct SolverPanel {
    bool available = false;        // The machine (or the tier override) supports some solving
    std::string hardware_summary;  // e.g. "4 threads, 15.6 GB free: solves river and turn, flop approximate"
    SolverTier tier = SolverTier::Unavailable;
    SolverProgress progress;       // For Hero's current decision; Off when there is none or the mode is off
};

// Everything a front end needs for one frame, with hidden information already removed.
// A REST or WebSocket server could serialize this as is.
struct TableSnapshot {
    ControllerPhase phase = ControllerPhase::BotToAct;
    int hand_number = 0;
    Street street = Street::Preflop;
    int small_blind = 0;
    int big_blind = 0;
    std::array<SeatSnapshot, kMaxSeats> seats;
    std::vector<Card> board;
    std::optional<std::string> waiting_on;

    LivePots pots;
    bool showdown = false;
    std::vector<PayoutLine> payouts;  // Filled once the hand is complete
    std::optional<std::string> refund_text;

    // Set only while it is Hero's turn.
    LegalActions legal;
    int current_bet = 0;
    int raise_cap_to = 0;  // The most any opponent can match, within what is legal
    std::optional<HintRecommendation> hint;

    std::optional<std::string> hero_hand;  // From the flop on, until Hero folds, e.g. "Pair of Kings"
    bool can_fold_early = false;           // A bot is to act and Hero could still fold
    bool early_fold_armed = false;
    bool bots_revealed = false;

    std::vector<std::string> action_log;       // This hand, oldest first
    std::vector<HeroMistake> hand_mistakes;    // Noted so far in this hand
    SessionStats session;
    std::optional<std::string> log_error;      // Logging failed; play can continue
    SolverPanel solver;
};

// The chips a raise reaches for a share of the pot, clamped to what is legal. It sizes
// from the pot after calling, so 1.0 is a standard pot-sized raise, and stops at
// raise_cap_to, so it gives the same chips as the advisor's size. Returns 0 if neither
// a bet nor a raise is legal.
int quick_bet_to(const LegalActions& legal, int current_bet, int total_pot, double pot_fraction, int raise_cap_to);

// The inverse of quick_bet_to: the size of a bet or raise to `bet_to`, as a whole
// percentage of the pot after calling.
int bet_pot_percent(const LegalActions& legal, int current_bet, int total_pot, int bet_to);

// The boundary between the game and any front end. The GUI uses only this, so it could
// be replaced by a server without touching engine, bot or advisor code.
class IGameController {
public:
    virtual ~IGameController() = default;

    virtual const TableSnapshot& snapshot() const = 0;

    // Advances bot pacing; call once a frame with the elapsed time.
    virtual void tick(double elapsed_ms) = 0;

    // Returns false, changing nothing, unless it is Hero's turn and the action is legal.
    [[nodiscard]] virtual bool submit_hero_action(const Action& action) = 0;

    // Deals the next hand. Returns false unless the last hand is complete and the
    // session continues.
    [[nodiscard]] virtual bool next_hand() = 0;

    // Arms or cancels a fold that happens when action reaches Hero, unless Hero could
    // check for free, which cancels it instead. While armed, bots act without their
    // delay. Arming returns false, changing nothing, unless can_fold_early is set.
    [[nodiscard]] virtual bool set_early_fold(bool armed) = 0;

    // Shows every bot's cards for the finished hand, until the next one is dealt.
    // Returns false during a hand.
    [[nodiscard]] virtual bool reveal_bot_hands() = 0;

    // Ends the current freezeout, even mid-hand, and starts a new one.
    virtual void new_session() = 0;

    virtual void set_bot_delay_ms(double delay_ms) = 0;
    virtual double bot_delay_ms() const = 0;
    virtual void set_instant_play(bool instant) = 0;
    virtual bool instant_play() const = 0;

    // Solver mode: a background solve of Hero's heads-up postflop decisions, shown
    // beside the heuristic hint (which stays the advice that mistakes are judged on).
    // Turning it on solves the current decision; turning it off cancels.
    virtual void set_solver_enabled(bool enabled) = 0;
    virtual bool solver_enabled() const = 0;
    // The time budget of each solve, from the next decision on.
    virtual void set_solver_budget_ms(int budget_ms) = 0;
    virtual int solver_budget_ms() const = 0;
};

}  // namespace poker
