#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "advisor/training_advisor.h"
#include "engine/game_types.h"
#include "engine/player_view.h"

namespace poker {

// A hint counts as ignored only if following it was estimated to be worth at least
// this many big blinds more. Close decisions are not mistakes, and the advisor's EVs
// are heuristic, so smaller gaps would mostly be noise.
inline constexpr double kMistakeEvGapBigBlinds = 1.0;

// Hero's decision where it deviated significantly from the hint.
struct HeroMistake {
    Street street = Street::Preflop;
    std::string taken;        // Action code as in the hand log, e.g. "C40"
    std::string recommended;  // The hint's best_action text
    double equity_pct = 0.0;
    double pot_odds_pct = 0.0;
    double mdf_pct = 0.0;
    double ev_loss = 0.0;  // Estimated chips given up against the hint
    std::string rationale;
};

// Compares Hero's chosen action with the hint computed for the same view. Returns a
// mistake if Hero folded when checking was free, or gave up at least
// kMistakeEvGapBigBlinds of estimated EV. A bet size between the advisor's options is
// valued as the nearest one. If an EV is unavailable for either action, a change of
// action type counts as a mistake, with ev_loss 0.
std::optional<HeroMistake> assess_hero_decision(const PlayerView& view, const HintRecommendation& hint,
                                                const Action& taken);

// A new id for a session, e.g. "20261101T120000Z-a3f9": UTC start time plus a random
// suffix, so sessions started within the same second stay distinct across runs.
std::string make_session_id();

struct HandLogContext {
    std::string session_id;  // Joins hand lines to their session summary line
    int small_blind = 0;
    int big_blind = 0;
    std::array<std::string, kMaxSeats> seat_labels;  // e.g. "Hero", "Nit", "Shift:GTO"
    std::optional<int> hero_seat;
    std::vector<HeroMistake> hero_mistakes;
};

// One finished hand as a single line of JSON with no trailing newline:
//
//   session_id       The session this hand belongs to; h restarts in each session
//   h, sb, bb, btn   Hand number, configured blinds, button seat index
//   post             Blinds actually posted, {sb:{s,amt}, bb:{s,amt}}; a short stack
//                    can post less than sb or bb, and heads-up the BTN posts sb
//   seats            Players dealt in: {s, p, pos, st, c}. s is the seat index, p the
//                    label, st the stack before blinds. c is present for anyone who
//                    saw a flop, and always for Hero so mistakes can be inspected
//   pre,flop,turn,riv  {b, pot, a}, present once the street is reached. b is the new
//                    board cards (absent preflop), pot the chips entering the street
//                    (blinds preflop), and a the voluntary actions "POS:ActionAmount"
//   win              {p, amt, hand}; hand only if the pot went to showdown
//   ref              {s, amt}, only if an uncalled bet was returned; not part of win
//   hero_mistakes    {st, did, rec, eq, odds, mdf, ev_loss, why}, one per decision
//
// Actions are F fold, X check, C call, B bet, R raise and A all-in. Amounts for C are
// chips added, and for B, R and A the player's total for the street after acting.
//
// Throws std::logic_error if the state has no finished hand.
std::string serialize_hand(const GameState& state, const HandLogContext& context);

// Appends hands to a JSONL file, by default logs/hand_history.jsonl.
class HandLogger {
public:
    // Creates parent directories as needed. Throws std::runtime_error if the file
    // cannot be opened for appending.
    explicit HandLogger(const std::filesystem::path& path = "logs/hand_history.jsonl");

    // Call for each Hero decision with the view and hint from before the action.
    // Returns the mistake it noted for the current hand, if any.
    std::optional<HeroMistake> record_hero_decision(const PlayerView& view, const HintRecommendation& hint,
                                                    const Action& taken);

    // Writes the finished hand with the noted mistakes, flushes so a crash loses
    // nothing, and returns those mistakes for SessionLogger. Any mistakes in
    // `context` are replaced. Throws std::runtime_error if the write fails.
    std::vector<HeroMistake> write_hand(const GameState& state, HandLogContext context);

    // Drops mistakes noted for a hand that will never be written, such as one
    // abandoned by resetting the session.
    void discard_pending_mistakes() { pending_mistakes_.clear(); }

private:
    std::ofstream out_;
    std::vector<HeroMistake> pending_mistakes_;
};

// Appends one summary line per session, by default to logs/session_summary.jsonl:
//
//   session_id  Same value as on the session's hand lines
//   ts          UTC time the session was written
//   sb, bb      Configured blinds
//   hands       Hands played
//   done        True if the freezeout finished (Hero busted or one player left)
//   winner      Winner's label, only if one player has all the chips
//   seats       {s, p, st, end, bust}: start and final stack; bust is the hand number
//               the seat busted on, only if it did
//   hero        {s, net, bust, mistakes, ev_loss, by_street:{pre,flop,turn,riv}},
//               only if there is a Hero; by_street counts mistakes per street
class SessionLogger {
public:
    // Seats start from `starting_stacks`, typically read from the engine before the
    // first hand. Creates parent directories; throws std::runtime_error on failure.
    SessionLogger(const std::array<int, kMaxSeats>& starting_stacks, const std::filesystem::path& path =
                                                                         "logs/session_summary.jsonl");

    void record_hand_mistakes(const std::vector<HeroMistake>& mistakes);

    // Uses the context's blinds, labels and Hero seat; pass Bot 5's public name, as
    // a per-hand "Shift:" label means nothing for a whole session. `timestamp`
    // overrides the clock, for reproducible output. Throws std::runtime_error if the
    // write fails.
    void write_session(const SessionSummary& summary, const HandLogContext& context,
                       std::optional<std::string> timestamp = std::nullopt);

    int mistakes_recorded() const { return mistakes_; }
    double ev_loss_recorded() const { return ev_loss_; }

private:
    std::array<int, kMaxSeats> starting_stacks_;
    std::ofstream out_;
    int mistakes_ = 0;
    double ev_loss_ = 0.0;
    std::array<int, 4> mistakes_by_street_{};
};

}  // namespace poker
