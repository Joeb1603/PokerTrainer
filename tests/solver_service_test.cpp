#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

#include "controller/solver_service.h"
#include "engine/game_engine.h"
#include "range/combos.h"
#include "range/range_model.h"
#include "solver/hardware.h"
#include "solver/postflop_solver.h"
#include "solver/spot_from_view.h"

using namespace poker;
using Clock = std::chrono::steady_clock;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

// Heads-up, 100 BB: the button (0, small blind) opens to 60 and the big blind (1)
// calls; then both check until `street`, where the big blind is first to act.
// Street::Preflop leaves the button to act preflop instead.
PlayerView heads_up_view(Street street, int stack = 2000, int button_stack = 0, int river_bet = 0) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{button_stack > 0 ? button_stack : stack, stack, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = std::array<Card, 2>{card("Ah"), card("Td")};
    deal.holes[1] = std::array<Card, 2>{card("Ks"), card("Qs")};
    deal.board = {card("Kd"), card("7c"), card("2h"), card("4s"), card("9d")};
    engine.start_hand(deal);
    if (street == Street::Preflop) return engine.player_view(0);
    EXPECT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    while (engine.state().street < street) {
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    EXPECT_EQ(engine.state().street, street);
    EXPECT_EQ(engine.state().to_act, 1);
    if (river_bet > 0) {  // The big blind checks and the button bets
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Bet, river_bet}));
    }
    return engine.player_view(1);
}

SolveRequest request_for(const PlayerView& view, SolverTier tier, std::uint64_t epoch) {
    SolveRequest request;
    request.key = {epoch, view.hand_number, view.actions.size()};
    request.view = view;
    request.tier = tier;
    request.budget_ms = 60'000;
    request.threads = 2;
    request.memory_bytes = std::size_t{1} << 30;
    return request;
}

// Polls until `key` leaves Running (or the time runs out); returns the last answer.
std::optional<SolverProgress> wait_for(const SolverService& service, const SolveKey& key, double timeout_ms) {
    const Clock::time_point start = Clock::now();
    std::optional<SolverProgress> progress;
    while (ms_since(start) < timeout_ms) {
        progress = service.poll(key);
        if (progress && progress->state != SolverState::Running) return progress;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return progress;
}

// Polls until `key` is solving: past range inference, the solver's setup and the
// first iteration.
bool wait_until_solving(const SolverService& service, const SolveKey& key, double timeout_ms) {
    const Clock::time_point start = Clock::now();
    while (ms_since(start) < timeout_ms) {
        const auto progress = service.poll(key);
        if (progress && progress->state == SolverState::Running && progress->iterations > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool legal(const LegalActions& legal, const Action& action) {
    if (!legal.allows(action.type)) return false;
    if (action.type == ActionType::Bet || action.type == ActionType::Raise) {
        return action.amount >= legal.min_raise_to && action.amount <= legal.max_raise_to;
    }
    return true;
}

}  // namespace

// --- Hardware tiers ----------------------------------------------------------------

TEST(Hardware, TierThresholds) {
    const auto info = [](unsigned threads, double avail_gb) {
        HardwareInfo h;
        h.threads = threads;
        h.avail_ram = static_cast<std::uint64_t>(avail_gb * static_cast<double>(kGiB));
        h.total_ram = std::max(h.avail_ram, 16 * kGiB);
        return h;
    };
    EXPECT_EQ(choose_tier(info(1, 64)), SolverTier::Unavailable);
    EXPECT_EQ(choose_tier(info(16, 0.99)), SolverTier::Unavailable);
    EXPECT_EQ(choose_tier(info(2, 1.0)), SolverTier::River);
    EXPECT_EQ(choose_tier(info(3, 32)), SolverTier::River);
    EXPECT_EQ(choose_tier(info(4, 3.99)), SolverTier::River);
    EXPECT_EQ(choose_tier(info(4, 4.0)), SolverTier::Turn);
    EXPECT_EQ(choose_tier(info(7, 64)), SolverTier::Turn);
    EXPECT_EQ(choose_tier(info(8, 7.99)), SolverTier::Turn);
    EXPECT_EQ(choose_tier(info(8, 8.0)), SolverTier::Flop);
    EXPECT_EQ(choose_tier(info(32, 128)), SolverTier::Flop);
    HardwareInfo unknown = info(4, 0);
    unknown.ram_known = false;
    EXPECT_EQ(choose_tier(unknown), SolverTier::River);
    unknown.threads = 1;
    EXPECT_EQ(choose_tier(unknown), SolverTier::Unavailable);

    // Budget: half the available memory, at most 16 GB; threads: all but one.
    EXPECT_EQ(solver_memory_budget(info(8, 10)), 5 * kGiB);
    EXPECT_EQ(solver_memory_budget(info(8, 100)), 16 * kGiB);
    EXPECT_EQ(solver_threads(info(1, 8)), 1);
    EXPECT_EQ(solver_threads(info(2, 8)), 1);
    EXPECT_EQ(solver_threads(info(8, 8)), 7);

    // Street to tree.
    EXPECT_FALSE(tree_for(SolverTier::Flop, Street::Preflop, true));
    EXPECT_FALSE(tree_for(SolverTier::Unavailable, Street::River, true));
    EXPECT_EQ(tree_for(SolverTier::River, Street::River, false), SolverTree::River);
    EXPECT_FALSE(tree_for(SolverTier::River, Street::Turn, true));
    EXPECT_FALSE(tree_for(SolverTier::River, Street::Flop, true));
    EXPECT_EQ(tree_for(SolverTier::Turn, Street::Turn, false), SolverTree::Turn);
    EXPECT_EQ(tree_for(SolverTier::Turn, Street::Flop, true), SolverTree::FlopCheckDown);
    EXPECT_EQ(tree_for(SolverTier::Flop, Street::Flop, true), SolverTree::FlopFull);
    EXPECT_EQ(tree_for(SolverTier::Flop, Street::Flop, false), SolverTree::FlopCheckDown);
    EXPECT_EQ(street_label(SolverTree::FlopCheckDown), "Flop (approximate: no river betting)");
    EXPECT_TRUE(abstraction_for(SolverTree::FlopCheckDown).check_down_river);
    EXPECT_DOUBLE_EQ(target_exploitability_pct(SolverTree::Turn), 0.8);

    EXPECT_EQ(hardware_summary(info(4, 15.6), SolverTier::Turn), "4 threads, 15.6 GB free: solves river and turn, flop approximate");
    EXPECT_EQ(hardware_summary(info(1, 0.5), SolverTier::Unavailable),
              "1 thread, 0.5 GB free: solver unavailable (needs 2 threads and 1 GB free)");
}

TEST(Hardware, DetectReturnsSomethingSane) {
    const HardwareInfo info = detect_hardware();
    EXPECT_GE(info.threads, 1u);
    EXPECT_LE(info.threads, 4096u);
#if defined(_WIN32) || defined(__linux__)
    ASSERT_TRUE(info.ram_known);
    EXPECT_GE(info.total_ram, std::uint64_t{256} << 20);
    EXPECT_GT(info.avail_ram, 0u);
    EXPECT_LE(info.avail_ram, info.total_ram);
#endif
    EXPECT_FALSE(hardware_summary(info, choose_tier(info)).empty());
}

// --- The service ---------------------------------------------------------------------

// A finished river solve answers its own key only, and a replaced or cancelled job's
// result is gone. The advice is the solver's root strategy for Hero's own combo.
TEST(SolverService, ResultMatchesKeyAndStaleIsRejected) {
    const PlayerView river = heads_up_view(Street::River);
    SolverService service;
    SolveRequest first = request_for(river, SolverTier::River, 1);
    first.fixed_iterations = 60;
    service.submit(first);
    SolveKey other = first.key;
    other.epoch = 2;
    EXPECT_FALSE(service.poll(other));
    SolveKey later_action = first.key;
    ++later_action.action_count;
    EXPECT_FALSE(service.poll(later_action));

    const auto done = wait_for(service, first.key, 20'000);
    ASSERT_TRUE(done);
    ASSERT_EQ(done->state, SolverState::Done) << done->note;
    EXPECT_EQ(done->iterations, 60);
    ASSERT_TRUE(done->advice);
    ASSERT_TRUE(done->exploitability_pct);
    EXPECT_EQ(done->advice->street_label, "River");
    EXPECT_FALSE(service.poll(other));

    // The same solve by hand: Hero's combo's root frequencies.
    auto villains = infer_ranges(river, Archetype::Baseline);
    WeightedRange hero = infer_range_for(river, 1, Archetype::Baseline, false);
    const int combo = combo_index(card("Ks"), card("Qs"));
    hero.set(combo, std::max(hero.weight(combo), 0.01f));
    SolverLimits limits;
    limits.memory_bytes = first.memory_bytes;
    PostflopSolver solver(spot_from_view(river, hero, *villains[0]), river_default_abstraction(), limits);
    solver.run(60);
    const RootSolution root = solver.root();
    const auto h = static_cast<std::size_t>(std::find(root.hands.begin(), root.hands.end(), combo) - root.hands.begin());
    ASSERT_LT(h, root.hands.size());
    double total = 0.0;
    double best = -1.0;
    for (const SolverOption& option : done->advice->options) {
        EXPECT_TRUE(legal(river.legal, option.action)) << option.label;
        total += option.freq;
        best = std::max(best, option.freq);
        bool matched = false;
        for (std::size_t a = 0; a < root.actions.size(); ++a) {
            if (option.freq == static_cast<double>(root.freq[a][h]) && option.ev_chips == static_cast<double>(root.ev_best[a][h])) {
                matched = true;
            }
        }
        EXPECT_TRUE(matched) << option.label;
    }
    EXPECT_EQ(done->advice->options.size(), root.actions.size());
    EXPECT_NEAR(total, 1.0, 1e-4);
    const auto chosen = std::find_if(done->advice->options.begin(), done->advice->options.end(), [&](const SolverOption& o) {
        return o.action.type == done->advice->best.type && o.action.amount == done->advice->best.amount;
    });
    ASSERT_NE(chosen, done->advice->options.end());
    EXPECT_EQ(chosen->freq, best);
    EXPECT_EQ(chosen->label, done->advice->best_label);
    EXPECT_EQ(done->advice->options.front().label, "CHECK");

    // A new submit replaces the finished result; cancel clears even the new key.
    SolveRequest second = first;
    second.key = other;
    service.submit(second);
    EXPECT_FALSE(service.poll(first.key));
    ASSERT_TRUE(service.poll(other));
    service.cancel();
    EXPECT_FALSE(service.poll(other));
    EXPECT_FALSE(service.poll(first.key));
}

// Facing a bet from a shorter stack: fold, call and raises, where the tree's all-in
// (the shorter stack's) is a raise to that amount at the table. Every option is legal.
TEST(SolverService, FacingABetMapsEveryOptionToALegalAction) {
    const PlayerView view = heads_up_view(Street::River, 3000, 1000, 100);
    ASSERT_TRUE(view.legal.allows(ActionType::Raise));
    SolveRequest request = request_for(view, SolverTier::River, 1);
    request.fixed_iterations = 30;
    const SolverProgress result = SolverService::solve(request, {});
    ASSERT_EQ(result.state, SolverState::Done) << result.note;
    ASSERT_TRUE(result.advice);
    bool fold = false;
    bool call = false;
    int raises = 0;
    int biggest = 0;
    for (const SolverOption& option : result.advice->options) {
        EXPECT_TRUE(legal(view.legal, option.action)) << option.label;
        fold |= option.action.type == ActionType::Fold;
        call |= option.action.type == ActionType::Call && option.label == "CALL 100";
        if (option.action.type == ActionType::Raise) {
            ++raises;
            biggest = std::max(biggest, option.action.amount);
            EXPECT_EQ(option.label.rfind("RAISE to " + std::to_string(option.action.amount) + " (", 0), 0u) << option.label;
        }
        EXPECT_NE(option.action.type, ActionType::AllIn) << option.label;  // Hero covers the button
    }
    EXPECT_TRUE(fold);
    EXPECT_TRUE(call);
    EXPECT_GE(raises, 2);
    EXPECT_EQ(biggest, 940);  // The button's whole stack: 1000 less the 60 preflop
}

TEST(SolverService, NotApplicableSpotsSayWhy) {
    SolverService service;
    const auto run = [&](const PlayerView& view, SolverTier tier, std::uint64_t epoch) {
        SolveRequest request = request_for(view, tier, epoch);
        request.fixed_iterations = 10;
        service.submit(request);
        const auto result = wait_for(service, request.key, 20'000);
        EXPECT_TRUE(result);
        return result.value_or(SolverProgress{});
    };
    SolverProgress p = run(heads_up_view(Street::Preflop), SolverTier::Flop, 1);
    EXPECT_EQ(p.state, SolverState::NotApplicable);
    EXPECT_NE(p.note.find("preflop"), std::string::npos) << p.note;
    p = run(heads_up_view(Street::Turn), SolverTier::River, 2);
    EXPECT_EQ(p.state, SolverState::NotApplicable);
    EXPECT_NE(p.note.find("Turn needs"), std::string::npos) << p.note;
    p = run(heads_up_view(Street::River), SolverTier::Unavailable, 3);
    EXPECT_EQ(p.state, SolverState::NotApplicable);

    SolveRequest tiny = request_for(heads_up_view(Street::River), SolverTier::River, 4);
    tiny.memory_bytes = 1000;
    service.submit(tiny);
    p = wait_for(service, tiny.key, 20'000).value_or(SolverProgress{});
    EXPECT_EQ(p.state, SolverState::NotApplicable);
    EXPECT_EQ(p.note.rfind("River: too big for this machine (needs", 0), 0u) << p.note;

    // The Flop tier takes the full flop tree only with the time and memory for it.
    SolveRequest flop = request_for(heads_up_view(Street::Flop), SolverTier::Flop, 5);
    flop.memory_bytes = 1000;
    for (const int budget : {8000, kFlopFullMinBudgetMs}) {
        flop.budget_ms = budget;
        ++flop.key.epoch;
        service.submit(flop);
        p = wait_for(service, flop.key, 20'000).value_or(SolverProgress{});
        EXPECT_EQ(p.note.rfind("Flop (approximate: no river betting): too big", 0), 0u) << p.note;
    }
    // With enough memory, the full tree's estimate is what is refused.
    const auto villains = infer_ranges(flop.view, Archetype::Baseline);
    const SolveSpot spot = spot_from_view(flop.view, infer_range_for(flop.view, 1, Archetype::Baseline, false), *villains[0]);
    flop.memory_bytes = PostflopSolver::estimate_bytes(spot, flop_full_abstraction(), flop.threads) - 1;
    ASSERT_GT(flop.memory_bytes, PostflopSolver::estimate_bytes(spot, flop_checkdown_abstraction(), flop.threads));
    ++flop.key.epoch;
    flop.budget_ms = kFlopFullMinBudgetMs;
    flop.fixed_iterations = 1;
    SolverProgress first;
    std::stop_source stop;
    (void)SolverService::solve(flop, stop.get_token(), [&](const SolverProgress& running) {
        if (first.note.empty()) first = running;
        stop.request_stop();
    });
    EXPECT_EQ(first.note, "Flop (approximate: no river betting)");
}

// A turn solve running on a long budget gives way to the next decision fast: the
// solver checks the job's stop between river cards, not only between iterations.
TEST(SolverService, NewDecisionCancelsQuickly) {
    SolverService service;
    const SolveRequest turn = request_for(heads_up_view(Street::Turn), SolverTier::Turn, 1);
    service.submit(turn);
    ASSERT_TRUE(wait_until_solving(service, turn.key, 30'000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Mid-iteration
    ASSERT_EQ(service.poll(turn.key)->state, SolverState::Running);

    // The next decision is one that needs no solving, so its answer times the cancel.
    const SolveRequest next = request_for(heads_up_view(Street::Preflop), SolverTier::Turn, 2);
    const Clock::time_point start = Clock::now();
    service.submit(next);
    const auto answer = wait_for(service, next.key, 5'000);
    const double latency = ms_since(start);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->state, SolverState::NotApplicable);
    // Measured at 6-8 ms in Release here. The bound leaves room for loaded
    // or sanitized test machines (216 ms under TSan with a bench running
    // alongside), and is still far below a full flop iteration (about 4 s).
    EXPECT_LT(latency, 1'000.0);
    std::printf("[ cancel latency: %.1f ms ]\n", latency);
    EXPECT_FALSE(service.poll(turn.key));
}

TEST(SolverService, DestructorJoinsMidSolve) {
    Clock::time_point start;
    {
        auto service = std::make_unique<SolverService>();
        const SolveRequest turn = request_for(heads_up_view(Street::Turn), SolverTier::Turn, 1);
        service->submit(turn);
        ASSERT_TRUE(wait_until_solving(*service, turn.key, 30'000));
        start = Clock::now();
        service.reset();
    }
    EXPECT_LT(ms_since(start), 500.0);
    // And straight after a submit, before the worker has picked the job up.
    start = Clock::now();
    {
        SolverService service;
        service.submit(request_for(heads_up_view(Street::Turn), SolverTier::Turn, 1));
    }
    EXPECT_LT(ms_since(start), 2'000.0);  // At most the job's range inference
}

// Exceptions inside a job become Failed with the message, and the worker carries on.
TEST(SolverService, ExceptionsBecomeFailed) {
    SolverService service;
    SolveRequest broken = request_for(heads_up_view(Street::River), SolverTier::River, 1);
    broken.fixed_iterations = 5;
    broken.view.button = -1;  // spot_from_view throws
    service.submit(broken);
    const auto failed = wait_for(service, broken.key, 20'000);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->state, SolverState::Failed);
    EXPECT_NE(failed->note.find("Solver failed"), std::string::npos) << failed->note;

    SolveRequest fine = request_for(heads_up_view(Street::River), SolverTier::River, 2);
    fine.fixed_iterations = 5;
    service.submit(fine);
    const auto done = wait_for(service, fine.key, 20'000);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->state, SolverState::Done) << done->note;
}

// End-to-end times for the stage notes: range inference, setup, iterations to the
// target or budget, the closing check and root(), on 3 threads. Flop full needs
// about 3.3 GB.
TEST(SolverService, DISABLED_ReportEndToEndTimes) {
    struct Case {
        const char* name;
        Street street;
        SolverTier tier;
        int budget_ms;
    };
    const std::array<Case, 4> cases{{{"river", Street::River, SolverTier::River, 8000},
                                     {"turn", Street::Turn, SolverTier::Turn, 30'000},
                                     {"flop check-down", Street::Flop, SolverTier::Turn, 30'000},
                                     {"flop full", Street::Flop, SolverTier::Flop, kFlopFullMinBudgetMs}}};
    for (const Case& c : cases) {
        SolveRequest request = request_for(heads_up_view(c.street), c.tier, 1);
        request.budget_ms = c.budget_ms;
        request.threads = 3;
        request.memory_bytes = std::size_t{6} << 30;
        {
            const PlayerView& view = request.view;
            const auto villains = infer_ranges(view, Archetype::Baseline);
            const SolveSpot spot = spot_from_view(view, infer_range_for(view, 1, Archetype::Baseline, false), *villains[0]);
            std::array<int, 2> hands{};
            for (std::size_t p = 0; p < 2; ++p) {
                for (int combo = 0; combo < kComboCount; ++combo) {
                    if (spot.ranges[p].weight(combo) > 0.0f) ++hands[p];
                }
            }
            const SolverTree tree = c.tier == SolverTier::Flop ? SolverTree::FlopFull
                                    : c.street == Street::Flop ? SolverTree::FlopCheckDown
                                    : c.street == Street::Turn ? SolverTree::Turn
                                                               : SolverTree::River;
            std::printf("%-16s %d x %d combos, estimate %.0f MB\n", c.name, hands[0], hands[1],
                        static_cast<double>(PostflopSolver::estimate_bytes(spot, abstraction_for(tree), 3)) / (1 << 20));
        }
        double first_publish = -1.0;
        const Clock::time_point start = Clock::now();
        const SolverProgress result = SolverService::solve(request, {}, [&](const SolverProgress& running) {
            if (first_publish < 0.0 && running.iterations > 0) first_publish = ms_since(start);
        });
        std::printf("%-16s %-9s first iteration %7.0f ms, total %7.0f ms, %4d iterations, exploitability %.2f%%, best %s\n", c.name,
                    result.state == SolverState::Done ? "done" : result.note.c_str(), first_publish, ms_since(start),
                    result.iterations, result.exploitability_pct.value_or(-1.0),
                    result.advice ? result.advice->best_label.c_str() : "-");

        // Cancel latency, mid-solve.
        SolverService service;
        service.submit(request);
        if (!wait_until_solving(service, request.key, 60'000)) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(c.street == Street::Flop && c.tier == SolverTier::Flop ? 3000 : 300));
        const SolveRequest next = request_for(heads_up_view(Street::Preflop), c.tier, 2);
        const Clock::time_point cancel = Clock::now();
        service.submit(next);
        (void)wait_for(service, next.key, 60'000);
        std::printf("%-16s cancel latency %.1f ms\n", c.name, ms_since(cancel));
    }
}
