#include "controller/solver_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

#include "range/combos.h"
#include "range/range_model.h"
#include "solver/postflop_solver.h"
#include "solver/spot_from_view.h"

namespace poker {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool contesting(SeatStatus status) { return status == SeatStatus::Active || status == SeatStatus::AllIn; }

SolverProgress not_applicable(std::string note) {
    SolverProgress progress;
    progress.state = SolverState::NotApplicable;
    progress.note = std::move(note);
    return progress;
}

std::string megabytes(std::size_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f MB", static_cast<double>(bytes) / static_cast<double>(1u << 20));
    return buffer;
}

bool is_legal(const LegalActions& legal, const Action& action) {
    if (!legal.allows(action.type)) return false;
    if (action.type == ActionType::Bet || action.type == ActionType::Raise) {
        return action.amount >= legal.min_raise_to && action.amount <= legal.max_raise_to;
    }
    return true;
}

// The table action for a root action of the tree, legal for the view, or nothing.
// The tree treats both players as having the shorter reach (street bet + stack), so
// its all-in is the shorter player's; with Hero deeper that is a bet or raise to
// that amount (at least the legal minimum, the excess being returned uncalled).
std::optional<Action> table_action(const PlayerView& view, const TreeAction& tree_action) {
    const LegalActions& legal = view.legal;
    std::optional<Action> action;
    switch (tree_action.type) {
        case ActionType::Fold: action = Action{ActionType::Fold, 0}; break;
        case ActionType::Check: action = Action{ActionType::Check, 0}; break;
        case ActionType::Call: action = Action{ActionType::Call, 0}; break;
        case ActionType::Bet:
        case ActionType::Raise:
        case ActionType::AllIn: {
            const ActionType raise_type = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
            if (!legal.allows(raise_type)) {
                // A short all-in faced without raising rights leaves only the all-in itself.
                if (legal.allows(ActionType::AllIn)) action = Action{ActionType::AllIn, 0};
                break;
            }
            const int to = std::clamp(tree_action.to, legal.min_raise_to, legal.max_raise_to);
            action = to >= legal.max_raise_to ? Action{ActionType::AllIn, 0} : Action{raise_type, to};
            break;
        }
    }
    if (action && !is_legal(legal, *action)) return std::nullopt;
    return action;
}

int chips_added(const PlayerView& view, const Action& action) {
    const PublicSeat& me = view.seats[static_cast<std::size_t>(view.seat)];
    switch (action.type) {
        case ActionType::Fold:
        case ActionType::Check: return 0;
        case ActionType::Call: return view.legal.call_amount;
        case ActionType::Bet:
        case ActionType::Raise: return action.amount - me.street_bet;
        case ActionType::AllIn: return me.stack;
    }
    return 0;
}

std::string label_of(const PlayerView& view, const Action& action) {
    const PublicSeat& me = view.seats[static_cast<std::size_t>(view.seat)];
    switch (action.type) {
        case ActionType::Fold: return "FOLD";
        case ActionType::Check: return "CHECK";
        case ActionType::Call: return "CALL " + std::to_string(view.legal.call_amount);
        case ActionType::AllIn: return "ALL-IN " + std::to_string(me.street_bet + me.stack);
        case ActionType::Bet:
        case ActionType::Raise: {
            // As bet_pot_percent: the share of the pot after calling.
            const int pot_after_call = view.total_pot + view.legal.call_amount;
            const long percent =
                pot_after_call > 0 ? std::lround(100.0 * (action.amount - view.current_bet) / pot_after_call) : 0;
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%s %d (%ld%% Pot)", action.type == ActionType::Bet ? "BET" : "RAISE to",
                          action.amount, percent);
            return buffer;
        }
    }
    return "";
}

}  // namespace

SolverProgress SolverService::solve(const SolveRequest& request, std::stop_token stop,
                                    const std::function<void(const SolverProgress&)>& publish) {
    const Clock::time_point start = Clock::now();
    const PlayerView& view = request.view;

    // --- Applicability, from the public table alone --------------------------------
    if (request.tier == SolverTier::Unavailable) {
        return not_applicable("Solver unavailable on this machine: showing heuristic advice");
    }
    if (view.street == Street::Preflop) return not_applicable("Postflop only: showing heuristic advice (preflop)");
    if (view.street == Street::Showdown || !view.to_act || *view.to_act != view.seat || !view.own_hole) {
        return not_applicable("No decision to solve");
    }
    int contesting_count = 0;
    int villain = -1;
    for (int index = 0; index < kMaxSeats; ++index) {
        if (!contesting(view.seats[static_cast<std::size_t>(index)].status)) continue;
        ++contesting_count;
        if (index != view.seat) villain = index;
    }
    if (contesting_count > 2) return not_applicable("Heads-up postflop only: showing heuristic advice (multiway)");
    if (contesting_count != 2 || villain < 0) return not_applicable("No opponent to solve against");
    if (view.seats[static_cast<std::size_t>(villain)].status == SeatStatus::AllIn) {
        return not_applicable("Opponent is all-in: showing heuristic advice");
    }

    // --- Ranges: the range model's Baseline reading of the public actions ----------
    const auto villain_ranges = infer_ranges(view, Archetype::Baseline);
    const std::optional<WeightedRange>& villain_range = villain_ranges[static_cast<std::size_t>(villain)];
    if (!villain_range || !(villain_range->total() > 0.0)) return not_applicable("No opponent range: showing heuristic advice");
    WeightedRange hero_range = infer_range_for(view, view.seat, Archetype::Baseline, /*remove_viewer_cards*/ false);
    // Hero's own cards must be in Hero's range, or the solve says nothing about them.
    const int hero_combo = combo_index((*view.own_hole)[0], (*view.own_hole)[1]);
    hero_range.set(hero_combo, std::max(hero_range.weight(hero_combo), 0.01f));
    const SolveSpot spot = spot_from_view(view, hero_range, *villain_range);

    // --- Tree and memory ----------------------------------------------------------
    const int threads = std::max(1, request.threads);
    bool flop_full_fits = false;
    const bool time_for_full = request.fixed_iterations > 0 || request.budget_ms >= kFlopFullMinBudgetMs;
    if (request.tier == SolverTier::Flop && view.street == Street::Flop && time_for_full) {
        flop_full_fits = PostflopSolver::estimate_bytes(spot, abstraction_for(SolverTree::FlopFull), threads) <=
                         request.memory_bytes;
    }
    const std::optional<SolverTree> tree = tree_for(request.tier, view.street, flop_full_fits);
    if (!tree) {
        return not_applicable(std::string(to_string(view.street)) + " needs a faster machine (this one solves " +
                              (request.tier == SolverTier::River ? "the river only" : "river and turn") +
                              "): showing heuristic advice");
    }
    const BetAbstraction abstraction = abstraction_for(*tree);
    const std::size_t required = PostflopSolver::estimate_bytes(spot, abstraction, threads);
    if (required > request.memory_bytes) {
        return not_applicable(std::string(street_label(*tree)) + ": too big for this machine (needs " + megabytes(required) +
                              ", budget " + megabytes(request.memory_bytes) + "): showing heuristic advice");
    }

    SolverProgress progress;
    progress.state = SolverState::Running;
    progress.note = std::string(street_label(*tree));
    SolverProgress cancelled;
    cancelled.state = SolverState::Off;
    cancelled.note = "Cancelled";

    Clock::time_point last_publish = Clock::now();
    const auto report = [&](bool force) {
        if (!publish) return;
        if (!force && ms_since(last_publish) < 25.0) return;
        last_publish = Clock::now();
        publish(progress);
    };
    report(true);  // The tree is known: the label shows during setup
    if (stop.stop_requested()) return cancelled;

    SolverLimits limits;
    limits.memory_bytes = request.memory_bytes;
    limits.threads = threads;
    limits.interrupt = stop;
    // The solver and its memory live only in this scope; the advice is copied out.
    PostflopSolver solver(spot, abstraction, limits);
    if (stop.stop_requested()) return cancelled;

    // --- Iterations ---------------------------------------------------------------
    const double budget_ms = std::max(1, request.budget_ms);
    const double target = target_exploitability_pct(*tree);
    const double check_every_ms = 0.05 * budget_ms;
    const double setup_ms = ms_since(start);

    if (request.fixed_iterations > 0) {
        while (solver.iterations() < request.fixed_iterations) {
            solver.run(std::min(10, request.fixed_iterations - solver.iterations()), stop);
            if (stop.stop_requested()) return cancelled;
            progress.iterations = solver.iterations();
            progress.progress = static_cast<double>(progress.iterations) / request.fixed_iterations;
            report(progress.iterations <= 10);
        }
        progress.exploitability_pct = solver.exploitability_pct_pot();
    } else {
        int since_check = 0;
        Clock::time_point last_check = Clock::now();
        double check_ms = 0.0;  // The last exploitability check's time
        bool converged = false;
        for (;;) {
            solver.run(1, stop);
            if (stop.stop_requested()) return cancelled;
            ++since_check;
            const double elapsed = ms_since(start);
            progress.iterations = solver.iterations();
            progress.progress = std::min(1.0, elapsed / budget_ms);
            // Stop early enough that the closing exploitability check and root() still fit
            // in the budget. Each is about two traversals, measured at 1.3 (turn) to 3.6
            // (flop check-down) iterations for the pair, so: two checks' measured time, or
            // three iterations before the first check.
            const double per_iteration = (elapsed - setup_ms) / solver.iterations();
            if (elapsed + std::max(3.0 * per_iteration, 2.0 * check_ms) >= budget_ms) break;
            if (since_check >= 10 && ms_since(last_check) >= check_every_ms) {
                const Clock::time_point check_start = Clock::now();
                progress.exploitability_pct = solver.exploitability_pct_pot();
                if (stop.stop_requested()) return cancelled;
                check_ms = ms_since(check_start);
                since_check = 0;
                last_check = Clock::now();
                report(true);
                if (*progress.exploitability_pct <= target) {
                    converged = true;
                    break;
                }
            } else {
                report(progress.iterations == 1);
            }
        }
        if (!converged) progress.exploitability_pct = solver.exploitability_pct_pot();
    }
    if (stop.stop_requested()) return cancelled;

    // --- Advice for Hero's own cards ----------------------------------------------
    const RootSolution root = solver.root();
    if (stop.stop_requested()) return cancelled;
    const auto found = std::find(root.hands.begin(), root.hands.end(), hero_combo);
    if (found == root.hands.end()) throw std::logic_error("solver: Hero's cards are not in Hero's range");
    const auto hand = static_cast<std::size_t>(found - root.hands.begin());

    SolverAdvice advice;
    advice.street_label = std::string(street_label(*tree));
    advice.target_pct = target;
    for (std::size_t a = 0; a < root.actions.size(); ++a) {
        const std::optional<Action> action = table_action(view, root.actions[a]);
        if (!action) continue;  // Not legal at the table (a raise the table does not reopen)
        const double freq = root.freq[a][hand];
        const double ev = root.ev_best[a][hand];
        // Two tree sizes can clamp to the same table action: they become one option.
        const auto same = std::find_if(advice.options.begin(), advice.options.end(), [&](const SolverOption& o) {
            return o.action.type == action->type && o.action.amount == action->amount;
        });
        if (same != advice.options.end()) {
            same->freq += freq;
            same->ev_chips = std::max(same->ev_chips, ev);
            continue;
        }
        advice.options.push_back({label_of(view, *action), *action, freq, ev});
    }
    if (advice.options.empty()) throw std::logic_error("solver: no root action is legal at the table");
    const SolverOption* best = &advice.options.front();
    for (const SolverOption& option : advice.options) {
        if (option.freq > best->freq ||
            (option.freq == best->freq && chips_added(view, option.action) < chips_added(view, best->action))) {
            best = &option;
        }
    }
    advice.best = best->action;
    advice.best_label = best->label;

    progress.state = SolverState::Done;
    progress.progress = 1.0;
    progress.iterations = solver.iterations();
    progress.advice = std::move(advice);
    return progress;
}

SolverService::SolverService() {
    worker_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

SolverService::~SolverService() {
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        current_key_.reset();
        ++current_serial_;
        current_stop_.request_stop();
    }
    worker_.request_stop();  // Wakes the wait
    if (worker_.joinable()) worker_.join();
}

void SolverService::submit(SolveRequest request) {
    {
        std::lock_guard lock(mutex_);
        current_stop_.request_stop();
        current_key_.reset();
        ++current_serial_;
        pending_ = std::move(request);
    }
    wake_.notify_one();
}

void SolverService::cancel() {
    std::lock_guard lock(mutex_);
    pending_.reset();
    current_stop_.request_stop();
    current_key_.reset();
    ++current_serial_;
}

std::optional<SolverProgress> SolverService::poll(const SolveKey& key) const {
    std::lock_guard lock(mutex_);
    if (pending_) {
        if (pending_->key != key) return std::nullopt;
        SolverProgress queued;
        queued.state = SolverState::Running;
        queued.note = "Starting";
        return queued;
    }
    if (current_key_ && *current_key_ == key) return current_progress_;
    return std::nullopt;
}

void SolverService::loop(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, stop, [&] { return pending_.has_value(); });
        if (stop.stop_requested() || !pending_) return;
        SolveRequest request = std::move(*pending_);
        pending_.reset();
        const std::uint64_t serial = ++current_serial_;
        current_key_ = request.key;
        current_stop_ = std::stop_source();
        const std::stop_token job_stop = current_stop_.get_token();
        current_progress_ = SolverProgress{};
        current_progress_.state = SolverState::Running;
        current_progress_.note = "Starting";
        lock.unlock();

        SolverProgress result;
        try {
            result = solve(request, job_stop, [&](const SolverProgress& progress) {
                std::lock_guard guard(mutex_);
                if (current_serial_ == serial) current_progress_ = progress;
            });
        } catch (const std::exception& error) {
            result = SolverProgress{};
            result.state = SolverState::Failed;
            result.note = std::string("Solver failed: ") + error.what();
        } catch (...) {
            result = SolverProgress{};
            result.state = SolverState::Failed;
            result.note = "Solver failed";
        }

        lock.lock();
        if (current_serial_ == serial) current_progress_ = std::move(result);
    }
}

}  // namespace poker
