#include "controller/local_game_controller.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

#include "bots/bots.h"
#include "bots/gto_heuristic_engine.h"
#include "controller/bot_names.h"

namespace poker {
namespace {

constexpr std::size_t at(int index) { return static_cast<std::size_t>(index); }

// The random lineup's own stream, apart from the engine, advisor and bot seeds.
constexpr std::uint64_t kLineupStream = 0x6A09E667F3BCC908ULL;

// Marks a showdown payout as a split, and notes the kicker when it beat a hand with the
// same name, using the pots the seat won and the five cards each shown hand plays.
void annotate_showdown_payout(PayoutLine& line, int seat, const HandResult& result,
                              const std::array<SeatSnapshot, kMaxSeats>& seats) {
    const std::optional<HandRank>& mine = result.showdown_ranks[at(seat)];
    if (!result.showdown || !mine || !seats[at(seat)].shown_five) return;

    std::optional<int> closest_loser;  // Same name, worse hand, nearest in rank
    for (const Pot& pot : result.pots) {
        if (std::find(pot.eligible.begin(), pot.eligible.end(), seat) == pot.eligible.end()) continue;
        HandRank best = kWorstHandRank + 1;
        int tied = 0;
        for (int other : pot.eligible) {
            const std::optional<HandRank>& rank = result.showdown_ranks[at(other)];
            if (!rank) continue;
            if (*rank < best) {
                best = *rank;
                tied = 0;
            }
            if (*rank == best) ++tied;
        }
        if (best != *mine) continue;  // This seat did not win this pot
        if (tied > 1) line.split = true;

        for (int other : pot.eligible) {
            const std::optional<HandRank>& rank = result.showdown_ranks[at(other)];
            if (!rank || *rank <= *mine || !seats[at(other)].shown_five) continue;
            if (describe_made_hand(*rank) != describe_made_hand(*mine)) continue;
            if (!closest_loser || *rank < *result.showdown_ranks[at(*closest_loser)]) closest_loser = other;
        }
    }

    if (closest_loser) {
        const auto diff = kicker_difference(*mine, *seats[at(seat)].shown_five,
                                            *result.showdown_ranks[at(*closest_loser)],
                                            *seats[at(*closest_loser)].shown_five);
        if (diff) line.kicker_note = rank_name(diff->winner) + " kicker beats " + rank_name(diff->loser);
    }
}

GameConfig make_config(const GameSetup& setup, std::uint64_t seed) {
    if (const auto problem = validate(setup)) throw std::invalid_argument("GameSetup: " + *problem);
    const BlindLevel first = setup.first_level();
    GameConfig config;
    config.starting_stack = setup.starting_stack();
    config.small_blind = first.small_blind;
    config.big_blind = first.big_blind;
    config.ante = first.ante;
    config.ante_type = first.ante_type;
    config.seed = seed;
    config.hero_seat = kHeroSeat;
    return config;
}

std::string level_text(const BlindLevel& level) {
    std::string text = std::to_string(level.small_blind) + "/" + std::to_string(level.big_blind);
    if (level.has_ante()) text += ", ante " + std::to_string(level.ante);
    return text;
}

std::string seat_text(const GameState& state, const std::array<std::string, kMaxSeats>& names, int seat) {
    const auto position = state.position_of(seat);
    return (position ? std::string(to_string(*position)) + " " : std::string()) + names[at(seat)];
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

std::string describe_payout(const PayoutLine& line) {
    std::string text = line.name + " wins " + std::to_string(line.amount);
    if (line.hand) text += " with " + *line.hand;
    std::vector<std::string> notes;
    if (line.split) notes.emplace_back("split pot");
    if (line.kicker_note) notes.push_back(*line.kicker_note);
    for (std::size_t i = 0; i < notes.size(); ++i) text += (i == 0 ? " (" : "; ") + notes[i];
    if (!notes.empty()) text += ")";
    return text;
}

int quick_bet_to(const LegalActions& legal, int current_bet, int total_pot, double pot_fraction, int raise_cap_to) {
    if (!legal.allows(ActionType::Bet) && !legal.allows(ActionType::Raise)) return 0;
    const double pot_after_call = total_pot + legal.call_amount;
    const int to = current_bet + static_cast<int>(std::lround(pot_fraction * pot_after_call));
    return std::clamp(std::min(to, raise_cap_to), legal.min_raise_to, legal.max_raise_to);
}

int bet_pot_percent(const LegalActions& legal, int current_bet, int total_pot, int bet_to) {
    const int pot_after_call = total_pot + legal.call_amount;
    if (pot_after_call <= 0) return 0;
    return static_cast<int>(std::lround(100.0 * (bet_to - current_bet) / pot_after_call));
}

LocalGameController::LocalGameController(const ControllerOptions& options)
    : options_(options),
      engine_(make_config(options.setup, options.seed)),
      advisor_(options.seed ^ 0xA5A5A5A5A5A5A5A5ull, options.advisor_rollouts),
      lineup_rng_(options.seed ^ kLineupStream),
      random_pending_(options.random_personalities),
      active_setup_(options.setup),
      pending_setup_(options.setup),
      bot_delay_ms_(options.bot_delay_ms),
      instant_play_(options.instant_play),
      hardware_(options.hardware_override ? *options.hardware_override : detect_hardware()),
      solver_tier_(options.solver_tier_override ? *options.solver_tier_override : choose_tier(hardware_)),
      solver_enabled_(options.solver_enabled),
      solver_budget_ms_(std::max(1, options.solver_budget_ms)) {
    solver_panel_.available = solver_tier_ != SolverTier::Unavailable;
    solver_panel_.tier = solver_tier_;
    solver_panel_.hardware_summary = hardware_summary(hardware_, solver_tier_);
    if (options.solver_tier_override) {
        solver_panel_.hardware_summary += " (tier set to " + std::string(to_string(solver_tier_)) + ")";
    }
    build_bots(random_pending_);
    for (int seat = 0; seat < kMaxSeats; ++seat) starting_stacks_[at(seat)] = engine_.state().seats[at(seat)].stack;
    reset_clock();

    try {
        hand_logger_.emplace(options.log_dir / "hand_history.jsonl");
    } catch (const std::exception& error) {
        note_log_failure(error);
    }
    open_session();
    begin_hand();
}

LocalGameController::~LocalGameController() { write_session_summary(); }

void LocalGameController::build_bots(bool random) {
    public_names_[kHeroSeat] = "Hero";
    random_active_ = random;
    if (!random) {
        // The classic lineup, seeded exactly as it always was.
        for (int seat = 1; seat < kMaxSeats; ++seat) {
            kinds_[at(seat)] = kAllBotKinds[at(seat - 1)];
            bots_[at(seat)] = make_bot(kinds_[at(seat)], options_.seed + 1000 + static_cast<std::uint64_t>(seat),
                                       options_.bot_rollouts);
            public_names_[at(seat)] = std::string(bots_[at(seat)]->name());
        }
        return;
    }
    // The draw order is the seat order, so the places are random too.
    const std::array<BotKind, 5> lineup = draw_random_lineup(lineup_rng_);
    const std::array<std::string, 5> names = draw_bot_names(lineup_rng_);
    for (int seat = 1; seat < kMaxSeats; ++seat) {
        kinds_[at(seat)] = lineup[at(seat - 1)];
        bots_[at(seat)] = make_bot(kinds_[at(seat)], lineup_rng_(), options_.bot_rollouts);
        public_names_[at(seat)] = names[at(seat - 1)];
    }
}

void LocalGameController::set_random_personalities(bool enabled) {
    random_pending_ = enabled;
    rebuild_snapshot();
}

bool LocalGameController::set_game_setup(const GameSetup& setup) {
    if (validate(setup)) return false;
    pending_setup_ = setup;
    rebuild_snapshot();
    return true;
}

void LocalGameController::set_clock_paused(bool paused) {
    clock_.set_paused(paused);
    rebuild_snapshot();
}

void LocalGameController::reset_clock() {
    clock_ = tournament() ? TournamentClock(active_setup_.schedule(), active_setup_.unit, active_setup_.level_length)
                          : TournamentClock();
}

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
    write_session_summary();  // Under the old names, before any new lineup
    if (hand_logger_) hand_logger_->discard_pending_mistakes();
    // The classic bots carry their state into the next session, as they always have;
    // a random lineup is drawn afresh, and leaving one rebuilds the classic bots.
    if (random_pending_ || random_active_) build_bots(random_pending_);
    // The RNG runs on either way, so a new game never replays the last one's deals.
    active_setup_ = pending_setup_;
    engine_.new_game(make_config(active_setup_, options_.seed));
    for (int seat = 0; seat < kMaxSeats; ++seat) starting_stacks_[at(seat)] = engine_.state().seats[at(seat)].stack;
    reset_clock();
    open_session();
    begin_hand();
}

bool LocalGameController::next_hand() {
    if (snapshot_.phase != ControllerPhase::HandComplete) return false;
    begin_hand();
    return true;
}

bool LocalGameController::set_early_fold(bool armed) {
    if (armed && !snapshot_.can_fold_early) return false;
    early_fold_armed_ = armed;
    rebuild_snapshot();
    return true;
}

bool LocalGameController::reveal_bot_hands() {
    if (snapshot_.phase != ControllerPhase::HandComplete && snapshot_.phase != ControllerPhase::SessionOver) return false;
    bots_revealed_ = true;
    rebuild_snapshot();
    return true;
}

void LocalGameController::begin_hand() {
    for (int seat = 0; seat < kMaxSeats; ++seat) hand_start_stacks_[at(seat)] = engine_.state().seats[at(seat)].stack;
    if (tournament()) engine_.set_level(clock_.on_hand_started());
    engine_.start_hand();
    hand_handled_ = false;
    early_fold_armed_ = false;
    bots_revealed_ = false;
    hand_mistakes_.clear();
    for (int seat = 1; seat < kMaxSeats; ++seat) bots_[at(seat)]->begin_hand(engine_.state().hand_number);
    bot_timer_ms_ = 0.0;
    after_state_change();
}

void LocalGameController::set_solver_enabled(bool enabled) {
    if (enabled == solver_enabled_) return;
    solver_enabled_ = enabled;
    update_solver(false, true);
    snapshot_.solver = solver_panel_;
}

void LocalGameController::update_solver(bool new_decision, bool solve) {
    if (new_decision) {
        solver_key_.reset();
        if (hero_view_) {
            ++decision_epoch_;
            solver_key_ = SolveKey{decision_epoch_, hero_view_->hand_number, hero_view_->actions.size()};
        }
    }
    if (!solve || !solver_enabled_ || !solver_key_ || !hero_view_) {
        solver_.cancel();
        solver_panel_.progress = SolverProgress{};
        return;
    }
    SolveRequest request;
    request.key = *solver_key_;
    request.view = *hero_view_;
    request.tier = solver_tier_;
    request.budget_ms = solver_budget_ms_;
    request.fixed_iterations = options_.solver_fixed_iterations;
    request.threads = options_.solver_threads > 0 ? options_.solver_threads : solver_threads(hardware_);
    request.memory_bytes = static_cast<std::size_t>(solver_memory_budget(hardware_));
    solver_.submit(std::move(request));
    solver_panel_.progress = SolverProgress{};
    solver_panel_.progress.state = SolverState::Running;
    solver_panel_.progress.note = "Starting";
}

void LocalGameController::poll_solver() {
    if (!solver_enabled_ || !solver_key_) return;
    if (auto progress = solver_.poll(*solver_key_)) {
        solver_panel_.progress = std::move(*progress);
        snapshot_.solver.progress = solver_panel_.progress;
    }
}

void LocalGameController::tick(double elapsed_ms) {
    poll_solver();  // First: Hero's turn returns early below
    if (tournament() && active_setup_.unit == LevelUnit::Minutes && snapshot_.phase != ControllerPhase::SessionOver &&
        !clock_.paused()) {
        clock_.advance_ms(elapsed_ms);
        if (snapshot_.tournament) snapshot_.tournament->seconds_until_next = clock_.ms_until_next_level() / 1000.0;
    }
    if (instant_play_ || early_fold_armed_) {
        // An armed fold ends once action reaches Hero, so normal pacing resumes after it.
        while (snapshot_.phase == ControllerPhase::BotToAct && (instant_play_ || early_fold_armed_)) {
            play_one_bot_action();
        }
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
    const Action action = bots_[at(seat)]->decide(engine_.player_view(seat));
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

    // The early fold is spent here either way: a free check goes back to Hero to decide.
    const bool fold_now = early_fold_armed_ && hero_view_ && !hero_view_->legal.allows(ActionType::Check);
    if (hero_view_ || state.phase != GamePhase::Betting) early_fold_armed_ = false;
    // A decision folded at once gets no solve.
    update_solver(true, !fold_now);
    rebuild_snapshot();
    if (fold_now) (void)submit_hero_action({ActionType::Fold});
}

HandLogContext LocalGameController::log_context(bool per_hand_labels) const {
    HandLogContext context;
    context.session_id = session_id_;
    // A hand is logged with its own blinds; the session with the first level's.
    const BlindLevel level = per_hand_labels ? engine_.state().level : active_setup_.first_level();
    context.small_blind = level.small_blind;
    context.big_blind = level.big_blind;
    if (tournament()) {
        context.level = clock_.level_index() + 1;
        if (!per_hand_labels) {
            TournamentLog log;
            log.place = hero_place(engine_.session_summary());
            log.unit = active_setup_.unit == LevelUnit::Minutes ? "min" : "hands";
            log.level_length = active_setup_.level_length;
            log.levels = active_setup_.schedule();
            context.tournament = std::move(log);
        }
    }
    context.hero_seat = kHeroSeat;
    context.seat_labels = public_names_;
    for (int seat = 1; seat < kMaxSeats; ++seat) {
        // A random lineup logs "Name:Style", e.g. "Marta:LAG" or "Marta:Shift:Nit"; the
        // session summary gives the public style, e.g. "Marta:Shifter".
        std::string& label = context.seat_labels[at(seat)];
        if (per_hand_labels) {
            const std::string style = bots_[at(seat)]->hand_log_label();
            label = random_active_ ? public_names_[at(seat)] + ":" + style : style;
        } else if (random_active_) {
            label = public_names_[at(seat)] + ":" + std::string(bot_kind_name(kinds_[at(seat)]));
        }
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
    if (tournament() && clock_.level_changed()) {
        log.push_back("Blinds up: level " + std::to_string(clock_.level_index() + 1) + ", " + level_text(state.level));
    }
    if (state.level.has_ante() && state.level.ante_type == AnteType::PerPlayer) {
        log.push_back("Everyone posts an ante of " + std::to_string(state.level.ante));
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            const Seat& s = state.seats[at(seat)];
            if (s.hole && s.ante_posted < state.level.ante) {
                log.push_back(seat_text(state, public_names_, seat) + " posts an ante of " +
                              std::to_string(s.ante_posted) + " (all-in)");
            }
        }
    }
    log.push_back(seat_text(state, public_names_, state.small_blind_seat) + " posts " +
                  std::to_string(state.small_blind_posted));
    log.push_back(seat_text(state, public_names_, state.big_blind_seat) + " posts " +
                  std::to_string(state.big_blind_posted));
    if (state.level.has_ante() && state.level.ante_type == AnteType::BigBlind) {
        const int paid = state.seats[at(state.big_blind_seat)].ante_posted;
        log.push_back(seat_text(state, public_names_, state.big_blind_seat) + " posts the ante, " + std::to_string(paid) +
                      (paid < state.level.ante ? " (all-in)" : ""));
    }

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
    next.small_blind = state.level.small_blind;
    next.big_blind = state.level.big_blind;
    next.ante = state.level.has_ante() ? state.level.ante : 0;
    next.ante_type = state.level.has_ante() ? state.level.ante_type : AnteType::None;
    next.mode = active_setup_.mode;
    next.setup_pending = !(pending_setup_ == active_setup_);
    if (tournament()) {
        TournamentPanel panel;
        panel.level = clock_.level_index() + 1;
        panel.current = clock_.current_level();
        panel.next = clock_.next_level();
        panel.unit = active_setup_.unit;
        panel.level_length = active_setup_.level_length;
        panel.hands_until_next = clock_.hands_until_next_level();
        panel.seconds_until_next = clock_.ms_until_next_level() / 1000.0;
        panel.paused = clock_.paused();
        panel.level_changed = clock_.level_changed();
        next.tournament = panel;
    }
    next.board = state.board;
    next.showdown = result && result->showdown;
    next.hand_mistakes = hand_mistakes_;
    next.action_log = build_action_log();
    next.log_error = log_error_;
    next.solver = solver_panel_;

    if (state.phase == GamePhase::SessionOver) {
        next.phase = ControllerPhase::SessionOver;
    } else if (finished) {
        next.phase = ControllerPhase::HandComplete;
    } else if (state.to_act == kHeroSeat) {
        next.phase = ControllerPhase::HeroToAct;
    } else {
        next.phase = ControllerPhase::BotToAct;
        next.waiting_on = public_names_[at(*state.to_act)];
    }

    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state.seats[at(seat)];
        SeatSnapshot& out = next.seats[at(seat)];
        out.name = public_names_[at(seat)];
        out.position = state.position_of(seat);
        out.stack = s.stack;
        out.street_bet = finished ? 0 : s.street_bet;  // Chips have moved to the winners
        out.status = s.status;
        out.is_button = seat == state.button;
        out.is_to_act = state.to_act == seat;
        out.holds_cards = s.hole.has_value() && s.status != SeatStatus::Folded;

        const bool shown = result && result->showdown_ranks[at(seat)].has_value();
        if (s.hole && (seat == kHeroSeat || shown)) out.visible_hole = s.hole;
        if (shown) out.shown_hand = describe_made_hand(*result->showdown_ranks[at(seat)]);
        if (shown && s.hole && state.board.size() == 5) {
            std::array<Card, 5> board{};
            std::copy(state.board.begin(), state.board.end(), board.begin());
            out.shown_five = best_five(*s.hole, board);
        }
        // Hero's own folded cards were never hidden from Hero, so they stay on the table,
        // dimmed, for the rest of the hand. Bots' folded cards wait for Reveal.
        if (s.hole && (bots_revealed_ || seat == kHeroSeat)) {
            out.visible_hole = s.hole;
            out.revealed_after_fold = s.status == SeatStatus::Folded;
        }
    }

    const Seat& hero = state.seats[kHeroSeat];
    if (hero.hole && hero.status != SeatStatus::Folded && state.board.size() >= 3) {
        std::vector<Card> cards(hero.hole->begin(), hero.hole->end());
        cards.insert(cards.end(), state.board.begin(), state.board.end());
        next.hero_hand = describe_made_hand(evaluate(cards));
    }
    next.can_fold_early = next.phase == ControllerPhase::BotToAct && hero.hole && hero.status == SeatStatus::Active;
    next.early_fold_armed = early_fold_armed_;
    next.bots_revealed = bots_revealed_;

    if (result) {
        // Returned chips were never at stake, so the final pot is what was paid out.
        next.pots.pots = result->pots;
        for (const Pot& pot : result->pots) next.pots.total += pot.amount;
        for (const Payout& payout : result->payouts) {
            next.seats[at(payout.seat)].won = payout.amount;
            PayoutLine line;
            line.name = public_names_[at(payout.seat)];
            line.amount = payout.amount;
            if (result->showdown_ranks[at(payout.seat)]) line.hand = describe_made_hand(*result->showdown_ranks[at(payout.seat)]);
            annotate_showdown_payout(line, payout.seat, *result, next.seats);
            next.payouts.push_back(std::move(line));
        }
        if (result->refund) {
            next.refund_text = "Uncalled " + std::to_string(result->refund->amount) + " returned to " +
                               public_names_[at(result->refund->seat)];
            next.action_log.push_back(*next.refund_text);
        }
        for (const PayoutLine& line : next.payouts) {
            next.action_log.push_back(describe_payout(line));
        }
    } else {
        next.pots = engine_.live_pots();
    }

    if (hero_view_) {
        next.legal = hero_view_->legal;
        next.current_bet = hero_view_->current_bet;
        next.raise_cap_to = clamp_raise_to(*hero_view_, hero_view_->legal.max_raise_to);
        next.hint = hero_hint_;
    }

    const SessionSummary summary = engine_.session_summary();
    next.session.session_id = session_id_;
    next.session.hands_played = summary.hands_played;
    next.session.hero_net = summary.final_stacks[kHeroSeat] - starting_stacks_[kHeroSeat];
    next.session.hero_busted = summary.hero_busted;
    if (summary.winner) next.session.winner = public_names_[at(*summary.winner)];
    next.session.mistakes = session_mistakes_ + (hand_handled_ ? 0 : static_cast<int>(hand_mistakes_.size()));
    next.session.ev_lost = session_ev_lost_;
    next.session.mode = active_setup_.mode;
    next.session.level_reached = tournament() ? clock_.level_index() + 1 : 1;
    next.session.hero_place = hero_place(summary);
    if (!hand_handled_) {
        for (const HeroMistake& mistake : hand_mistakes_) next.session.ev_lost += mistake.ev_loss;
    }
    if (random_active_ && next.phase == ControllerPhase::SessionOver) {
        for (int seat = 1; seat < kMaxSeats; ++seat) {
            next.session.lineup.push_back(public_names_[at(seat)] + ": " + std::string(bot_kind_name(kinds_[at(seat)])));
        }
    }
    next.random_personalities = random_pending_;
    next.random_lineup_active = random_active_;

    snapshot_ = std::move(next);
}

std::optional<int> LocalGameController::hero_place(const SessionSummary& summary) const {
    const GameState& state = engine_.state();
    if (state.phase != GamePhase::SessionOver) return std::nullopt;
    if (summary.winner == kHeroSeat) return 1;
    const Seat& hero = state.seats[kHeroSeat];
    if (hero.status != SeatStatus::Busted) return std::nullopt;
    // Hero busted on the latest hand: behind everyone left and everyone who busted on
    // the same hand from a bigger stack. A tie goes to Hero.
    int place = 1;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (seat == kHeroSeat) continue;
        const Seat& s = state.seats[at(seat)];
        const bool same_hand = s.busted_on_hand && s.busted_on_hand == hero.busted_on_hand;
        if (s.status != SeatStatus::Busted ||
            (same_hand && hand_start_stacks_[at(seat)] > hand_start_stacks_[kHeroSeat])) {
            ++place;
        }
    }
    return place;
}

}  // namespace poker
