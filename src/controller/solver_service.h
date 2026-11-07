#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "engine/player_view.h"
#include "engine/table_types.h"
#include "solver/hardware.h"

namespace poker {

// Names one Hero decision. The epoch is the controller's own count of decisions,
// because hand numbers restart with every session; the action count separates the
// decisions within a hand.
struct SolveKey {
    std::uint64_t epoch = 0;
    int hand_number = 0;
    std::size_t action_count = 0;

    friend bool operator==(const SolveKey&, const SolveKey&) = default;
};

struct SolveRequest {
    SolveKey key;
    PlayerView view;  // Hero's view at the decision: public table plus Hero's own cards
    SolverTier tier = SolverTier::River;
    int budget_ms = 8000;          // Wall time from the start of the job, setup included
    int fixed_iterations = 0;      // 0: the time budget; otherwise exactly this many (tests)
    int threads = 1;               // Solver threads, the worker included
    std::size_t memory_bytes = 0;  // The most a solve may hold
};

enum class SolverState : std::uint8_t {
    Off,            // Solver mode is off, or there is no decision to solve
    NotApplicable,  // This decision is not one the solver handles; `note` says why
    Running,
    Done,
    Failed,  // `note` holds the error
};

// One root action of Hero's: its average-strategy frequency for Hero's own cards and
// the chips it is worth (relative to now: received from the pot minus added from
// here, the hand then played as a best response; see RootSolution::ev_best).
struct SolverOption {
    std::string label;  // Like the advisor's: "CHECK", "BET 100 (50% Pot)", "CALL 150", ...
    Action action;      // Legal for the request's view
    double freq = 0.0;
    double ev_chips = 0.0;
};

struct SolverAdvice {
    std::vector<SolverOption> options;  // In the tree's root order
    Action best;  // The most frequent option; ties go to the cheaper action
    std::string best_label;
    std::string street_label;  // "River", "Turn", "Flop (approximate: no river betting)" or "Flop"
};

struct SolverProgress {
    SolverState state = SolverState::Off;
    double progress = 0.0;  // 0..1: of the time budget, or of the fixed iterations
    int iterations = 0;
    std::optional<double> exploitability_pct;  // Of the pot, at the last check
    std::string note;
    std::optional<SolverAdvice> advice;  // Done only
};

// Runs one solve at a time on a background thread, for the decision a key names.
//
// - One worker std::jthread waits on a condition_variable_any with its stop_token.
// - submit() cancels the job in progress (its own stop_source, which also interrupts
//   the solver mid-iteration: SolverLimits::interrupt) and replaces any job still
//   queued, so the worker always moves on to the newest decision.
// - poll() answers only for the key of the queued or current job; a finished job
//   whose key has been replaced or cancelled is gone, so a stale result can never
//   be shown. Progress and the result are published under one mutex, tagged with a
//   job serial, so a cancelled job's last writes are dropped.
// - The solver and its memory live only inside the job; only the advice is kept.
// - Exceptions become Failed with the message.
//
// The job sees only the request's PlayerView: ranges come from the range model
// (Baseline) and Hero's own cards are known only to Hero's side.
//
// Thread-safe: submit, cancel and poll may be called from any thread. The
// destructor cancels and joins.
class SolverService {
public:
    SolverService();
    ~SolverService();
    SolverService(const SolverService&) = delete;
    SolverService& operator=(const SolverService&) = delete;

    void submit(SolveRequest request);
    void cancel();
    std::optional<SolverProgress> poll(const SolveKey& key) const;

    // The whole job, synchronously, on the calling thread: what the worker runs. It
    // calls `publish` (if set) with Running progress as the solve goes: once the tree
    // is chosen (note = its street label, before the solver's setup), after the first
    // iteration, after each exploitability check, and otherwise at most about every
    // 25 ms. It returns the final value.
    // A stop abandons the solve and returns state Off with the note "Cancelled".
    static SolverProgress solve(const SolveRequest& request, std::stop_token stop,
                                const std::function<void(const SolverProgress&)>& publish = {});

private:
    void loop(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<SolveRequest> pending_;
    std::optional<SolveKey> current_key_;  // The job running or finished, unless replaced or cancelled
    std::uint64_t current_serial_ = 0;     // Bumped for each job, and by cancel()
    std::stop_source current_stop_;
    SolverProgress current_progress_;
    std::jthread worker_;  // Last: started after every other member exists
};

}  // namespace poker
