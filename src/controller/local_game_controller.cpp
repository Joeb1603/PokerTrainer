#include "controller/local_game_controller.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "bots/bots.h"

namespace poker {
namespace {

GameConfig make_config(const ControllerOptions& options) {
    GameConfig config;
    config.starting_stack = options.starting_stack;
    config.small_blind = options.small_blind;
    config.big_blind = options.big_blind;
    config.seed = options.seed;
    config.hero_seat = kHeroSeat;
    return config;
}

std::string seat_text(const GameState& state, const std::array<std::string, kMaxSeats>& names, int seat) {
    const auto position = state.position_of(seat);
    return (position ? std::string(to_string(*position)) + " " : std::string()) + names[seat];
}

std::string describe_action(const ActionRecord& a) {
    std::string text;
    switch (a.type) {
        case ActionType::Fold: text = "folds"; break;
        case ActionType::Check: text = "checks"; break;
        case ActionType::Call: text = "calls " + std::to_string(a.chips_added); break;
        case ActionType::Bet: text = "bets " + std::to_string(a.street_total); break;
        case ActionType::Raise: text = "raises to " + std::to_string(a.street_total); break;
        case ActionType::AllIn: break;  // Never recorded: resolves to the above
    }
    if (a.all_in) text += " (all-in)";
    return text;
}

}  // namespace

int quick_bet_to(const LegalActions& legal, int current_bet, int total_pot, double pot_fraction) {
    if (!legal.allows(ActionType::Bet) && !legal.allows(ActionType::Raise)) return 0;
    const double pot_after_call = total_pot + legal.call_amount;
    const int to = current_bet + static_cast<int>(std::lround(pot_fraction * pot_after_call));
    return std::clamp(to, legal.min_raise_to, legal.max_raise_to);
}

LocalGameController::LocalGameController(const ControllerOptions& options)
    : options_(options),
      engine_(make_config(options)),
      advisor_(options.seed ^ 0xA5A5A5A5A5A5A5A5ull, options.advisor_rollouts),
      bot_delay_ms_(options.bot_delay_ms),
      instant_play_(options.instant_play) {
    public_names_[kHeroSeat] = "Hero";
    for (int seat = 1; seat < kMaxSeats; ++seat) {
        bots_[seat] = make_bot(kAllBotKinds[seat - 1], options.seed + 1000 + seat, options.bot_rollouts);
        public_names_[seat] = std::string(bots_[seat]->name());
    }
    for (int seat = 0; seat < kMaxSeats; ++seat) starting_stacks_[seat] = engine_.state().seats[seat].stack;

    try {
        hand_logger_.emplace(options.log_dir / "hand_history.jsonl");
    } catch (const std::exception& error) {
        note_log_failure(error);
    }
    open_session();
    begin_hand();
}

LocalGameController::~LocalGameController() { write_session_summary(); }

void LocalGameController::note_log_failure(const std::exception& error) noexcept {
    try {
        log_error_ = error.what();
    } catch (...) {
    }
}

void LocalGameController::open_session() {
    session_id_ = make_session_id();
    session_written_ = false;
    session_mistakes_ = 0;
    session_ev_lost_ = 0.0;
    session_logger_.reset();
    try {
        session_logger_.emplace(starting_stacks_, options_.log_dir / "session_summary.jsonl");
    } catch (const std::exception& error) {
        note_log_failure(error);
    }
}

void LocalGameController::new_session() {
    write_session_summary();
    if (hand_logger_) hand_logger_->discard_pending_mistakes();
    engine_.new_game();
    open_session();
    begin_hand();
}

bool LocalGameController::next_hand() {
    if (snapshot_.phase != ControllerPhase::HandComplete) return false;
    begin_hand();
    return true;
}

void LocalGameController::begin_hand() {
    engine_.start_hand();
    hand_handled_ = false;
    hand_mistakes_.clear();
    for (int seat = 1; seat < kMaxSeats; ++seat) bots_[seat]->begin_hand(engine_.state().hand_number);
    bot_timer_ms_ = 0.0;
    after_state_change();
}

void LocalGameController::tick(double elapsed_ms) {
    if (instant_play_) {
        while (snapshot_.phase == ControllerPhase::BotToAct) play_one_bot_action();
        return;
    }
    if (snapshot_.phase != ControllerPhase::BotToAct) return;
    bot_timer_ms_ += elapsed_ms;
    if (bot_timer_ms_ >= bot_delay_ms_) {
        // One action a tick: a stalled frame should not make several bots act at once.
        bot_timer_ms_ = 0.0;
        play_one_bot_action();
    }
}

void LocalGameController::play_one_bot_action() {
    const int seat = *engine_.state().to_act;
    const Action action = bots_[seat]->decide(engine_.player_view(seat));
    if (!engine_.apply_action(action)) {
        // Bots are fuzz-tested never to do this. Folding keeps a bad bot from hanging the table.
        const LegalActions legal = engine_.legal_actions();
        (void)engine_.apply_action({legal.allows(ActionType::Check) ? ActionType::Check : ActionType::Fold});
    }
    after_state_change();
}

bool LocalGameController::submit_hero_action(const Action& action) {
    if (snapshot_.phase != ControllerPhase::HeroToAct || !hero_view_ || !hero_hint_) return false;

    // The view must be from before the action; the engine rejects illegal ones untouched.
    const PlayerView view = *hero_view_;
    const HintRecommendation hint = *hero_hint_;
    if (!engine_.apply_action(action)) return false;

    if (hand_logger_) {
        if (auto mistake = hand_logger_->record_hero_decision(view, hint, action)) hand_mistakes_.push_back(*mistake);
    } else if (auto mistake = assess_hero_decision(view, hint, action)) {
        hand_mistakes_.push_back(*mistake);
    }
    bot_timer_ms_ = 0.0;
    after_state_change();
    return true;
}

void LocalGameController::after_state_change() {
    const GameState& state = engine_.state();
    if (state.phase != GamePhase::Betting && !hand_handled_) on_hand_finished();

    hero_view_.reset();
    hero_hint_.reset();
    if (state.to_act == kHeroSeat) {
        hero_view_ = engine_.player_view(kHeroSeat);
        hero_hint_ = advisor_.analyze(*hero_view_);
    }
    rebuild_snapshot();
}

HandLogContext LocalGameController::log_context(bool per_hand_labels) const {
    HandLogContext context;
    context.session_id = session_id_;
    context.small_blind = options_.small_blind;
    context.big_blind = options_.big_blind;
    context.hero_seat = kHeroSeat;
    context.seat_labels = public_names_;
    if (per_hand_labels) {
        for (int seat = 1; seat < kMaxSeats; ++seat) context.seat_labels[seat] = bots_[seat]->hand_log_label();
    }
    return context;
}

void LocalGameController::on_hand_finished() {
    hand_handled_ = true;
    for (const HeroMistake& mistake : hand_mistakes_) {
        ++session_mistakes_;
        session_ev_lost_ += mistake.ev_loss;
    }

    try {
        if (hand_logger_) {
            const std::vector<HeroMistake> written = hand_logger_->write_hand(engine_.state(), log_context(true));
            if (session_logger_) session_logger_->record_hand_mistakes(written);
        } else if (session_logger_) {
            session_logger_->record_hand_mistakes(hand_mistakes_);
        }
    } catch (const std::exception& error) {
        note_log_failure(error);
    }

    if (engine_.state().phase == GamePhase::SessionOver) write_session_summary();
}

void LocalGameController::write_session_summary() noexcept {
    if (session_written_ || engine_.state().hand_number == 0) return;
    session_written_ = true;
    try {
        if (session_logger_) session_logger_->write_session(engine_.session_summary(), log_context(false));
    } catch (const std::exception& error) {
        note_log_failure(error);
    }
}

std::vector<std::string> LocalGameController::build_action_log() const {
    const GameState& state = engine_.state();
    std::vector<std::string> log;
    log.push_back(seat_text(state, public_names_, state.small_blind_seat) + " posts " +
                  std::to_string(state.small_blind_posted));
    log.push_back(seat_text(state, public_names_, state.big_blind_seat) + " posts " +
                  std::to_string(state.big_blind_posted));

    const auto add_street_header = [&](Street street) {
        const std::size_t count = street == Street::Flop ? 3 : street == Street::Turn ? 4 : 5;
        if (state.board.size() < count) return;
        std::string line = "-- " + std::string(to_string(street)) + ":";
        for (std::size_t i = street == Street::Flop ? 0 : count - 1; i < count; ++i) {
            line += " " + state.board[i].to_string();
        }
        line += " (pot " + std::to_string(state.street_start_pot[static_cast<std::size_t>(street)]) + ")";
        log.push_back(std::move(line));
    };

    Street current = Street::Preflop;
    for (const ActionRecord& action : state.actions) {
        while (current < action.street) {
            current = static_cast<Street>(static_cast<int>(current) + 1);
            add_street_header(current);
        }
        log.push_back(seat_text(state, public_names_, action.seat) + " " + describe_action(action));
    }
    if (state.last_result) {
        // Streets dealt with no action left, as in an all-in runout.
        while (current < Street::River) {
            current = static_cast<Street>(static_cast<int>(current) + 1);
            add_street_header(current);
        }
    }
    return log;
}

void LocalGameController::rebuild_snapshot() {
    const GameState& state = engine_.state();
    const bool finished = state.phase != GamePhase::Betting;
    const HandResult* result = finished && state.last_result ? &*state.last_result : nullptr;

    TableSnapshot next;
    next.hand_number = state.hand_number;
    next.street = state.street;
    next.small_blind = options_.small_blind;
    next.big_blind = options_.big_blind;
    next.board = state.board;
    next.showdown = result && result->showdown;
    next.hand_mistakes = hand_mistakes_;
    next.action_log = build_action_log();
    next.log_error = log_error_;

    if (state.phase == GamePhase::SessionOver) {
        next.phase = ControllerPhase::SessionOver;
    } else if (finished) {
        next.phase = ControllerPhase::HandComplete;
    } else if (state.to_act == kHeroSeat) {
        next.phase = ControllerPhase::HeroToAct;
    } else {
        next.phase = ControllerPhase::BotToAct;
        next.waiting_on = public_names_[*state.to_act];
    }

    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state.seats[seat];
        SeatSnapshot& out = next.seats[seat];
        out.name = public_names_[seat];
        out.position = state.position_of(seat);
        out.stack = s.stack;
        out.street_bet = finished ? 0 : s.street_bet;  // Chips have moved to the winners
        out.status = s.status;
        out.is_button = seat == state.button;
        out.is_to_act = state.to_act == seat;
        out.holds_cards = s.hole.has_value() && s.status != SeatStatus::Folded;

        const bool shown = result && result->showdown_ranks[seat].has_value();
        if (s.hole && (seat == kHeroSeat || shown)) out.visible_hole = s.hole;
        if (shown) out.shown_hand = describe_hand(*result->showdown_ranks[seat]);
    }

    if (result) {
        // Returned chips were never at stake, so the final pot is what was paid out.
        next.pots.pots = result->pots;
        for (const Pot& pot : result->pots) next.pots.total += pot.amount;
        for (const Payout& payout : result->payouts) {
            next.seats[payout.seat].won = payout.amount;
            PayoutLine line{public_names_[payout.seat], payout.amount, std::nullopt};
            if (result->showdown_ranks[payout.seat]) line.hand = describe_hand(*result->showdown_ranks[payout.seat]);
            next.payouts.push_back(std::move(line));
        }
        if (result->refund) {
            next.refund_text = "Uncalled " + std::to_string(result->refund->amount) + " returned to " +
                               public_names_[result->refund->seat];
            next.action_log.push_back(*next.refund_text);
        }
        for (const PayoutLine& line : next.payouts) {
            next.action_log.push_back(line.name + " wins " + std::to_string(line.amount) +
                                      (line.hand ? " with " + *line.hand : std::string()));
        }
    } else {
        next.pots = engine_.live_pots();
    }

    if (hero_view_) {
        next.legal = hero_view_->legal;
        next.current_bet = hero_view_->current_bet;
        next.hint = hero_hint_;
    }

    const SessionSummary summary = engine_.session_summary();
    next.session.session_id = session_id_;
    next.session.hands_played = summary.hands_played;
    next.session.hero_net = summary.final_stacks[kHeroSeat] - starting_stacks_[kHeroSeat];
    next.session.hero_busted = summary.hero_busted;
    if (summary.winner) next.session.winner = public_names_[*summary.winner];
    next.session.mistakes = session_mistakes_ + (hand_handled_ ? 0 : static_cast<int>(hand_mistakes_.size()));
    next.session.ev_lost = session_ev_lost_;
    if (!hand_handled_) {
        for (const HeroMistake& mistake : hand_mistakes_) next.session.ev_lost += mistake.ev_loss;
    }

    snapshot_ = std::move(next);
}

}  // namespace poker
