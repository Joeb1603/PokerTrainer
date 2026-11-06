// Scores the Training Advisor against the river solver on the benchmark spots in
// tools/bench/river_spots.json. Run from the repository root.
//
//   advisor_bench [--spots PATH] [--iterations N] [--min-iterations N] [--target PCT]
//                 [--combos N] [--advisor-rollouts N] [--seed S] [--timing] [--only ID]
//                 [--csv PATH] [--stability]
//
// For each spot: replay the script through GameEngine, solve the river from the
// spot's assumed ranges, then ask the advisor about a sample of hero combos and
// compare its action with the solver's. The tree is river_default_abstraction (root
// sizes are the advisor's 25/50/75/100% ladder plus all-in) with sizes merged into
// all-in only when they take the whole stack, as the advisor lists them, so every
// advisor bet or raise has a root action with the same street total. Per combo:
//   regret  = (max_a ev_best[a][h] - ev_best[advisor][h]) / big blind
//   agree   = the advisor's action is the solver's most frequent one
//   freq    = the solver's frequency for the advisor's action
// Each spot's figures are weighted by the sampled combos' root share (renormalised);
// the group tables average spots with equal weight.
//
// Exit status: 0 on success, 1 on a spot error, 2 on a usage or file error, and 3
// when any advisor action mapped inexactly to a root action (the tables still
// print, with the count in the "inexact maps" column).
//
// --stability continues each solve to twice the iterations it used and reports how
// much the sampled combos' regrets move: a check that ev_best has converged off
// the solver's own path. (The solver is deterministic, so continuing a solve is the
// same as re-solving at twice the iterations.)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/training_advisor.h"
#include "bench/bench_spots.h"
#include "core/hand_evaluator.h"
#include "range/combos.h"
#include "solver/betting_tree.h"
#include "solver/postflop_solver.h"
#include "solver/spot_from_view.h"

using namespace poker;

namespace {

constexpr int kCheckEvery = 25;  // Iterations between exploitability checks

constexpr std::string_view kUsage =
    "Usage: advisor_bench [--spots PATH] [--iterations N] [--min-iterations N] [--target PCT]\n"
    "                     [--combos N] [--advisor-rollouts N] [--seed S] [--timing] [--only ID]\n"
    "                     [--csv PATH] [--stability]\n"
    "  --spots             spot file (default tools/bench/river_spots.json)\n"
    "  --iterations        solver iteration cap per spot (default 3000)\n"
    "  --min-iterations    iterations before the target may stop a solve (default 2000, at\n"
    "                      most --iterations), so off-path ev_best values have converged\n"
    "                      even where exploitability falls fast\n"
    "  --target            stop once exploitability is at most PCT % of pot (default 0.3)\n"
    "  --combos            hero combos sampled per spot (default 80)\n"
    "  --advisor-rollouts  advisor Monte Carlo rollouts (default 5000)\n"
    "  --seed              advisor seed (default 0)\n"
    "  --timing            report advisor milliseconds per decision\n"
    "  --only              run one spot by id\n"
    "  --csv               write one row per sampled combo\n"
    "  --stability         re-solve each spot at twice its iterations and report the change\n"
    "                      in per-combo regret\n";

struct Options {
    std::string spots = "tools/bench/river_spots.json";
    int iterations = 3000;
    int min_iterations = 2000;
    double target = 0.3;
    int combos = 80;
    int rollouts = kDefaultRollouts;
    std::uint64_t seed = 0;
    bool timing = false;
    bool stability = false;
    std::optional<std::string> only;
    std::optional<std::string> csv;
};

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << "advisor_bench: " << message << "\n\n" << kUsage;
    std::exit(2);
}

int int_flag(std::string_view flag, std::string_view value, int minimum) {
    const std::optional<int> parsed = tools::parse_int(value);
    if (!parsed || *parsed < minimum) {
        usage_error(std::string(flag) + " needs an integer of at least " + std::to_string(minimum));
    }
    return *parsed;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--timing") {
            options.timing = true;
            continue;
        }
        if (arg == "--stability") {
            options.stability = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            std::exit(0);
        }
        if (i + 1 >= argc) usage_error("unknown flag or missing value: " + std::string(arg));
        const std::string_view value = argv[++i];
        if (arg == "--spots") {
            options.spots = std::string(value);
        } else if (arg == "--iterations") {
            options.iterations = int_flag(arg, value, 1);
        } else if (arg == "--min-iterations") {
            options.min_iterations = int_flag(arg, value, 0);
        } else if (arg == "--target") {
            const std::optional<double> target = tools::parse_double(value);
            if (!target || *target < 0.0) usage_error("--target needs a non-negative percentage");
            options.target = *target;
        } else if (arg == "--combos") {
            options.combos = int_flag(arg, value, 1);
        } else if (arg == "--advisor-rollouts") {
            options.rollouts = int_flag(arg, value, 1);
        } else if (arg == "--seed") {
            options.seed = static_cast<std::uint64_t>(int_flag(arg, value, 0));
        } else if (arg == "--only") {
            options.only = std::string(value);
        } else if (arg == "--csv") {
            options.csv = std::string(value);
        } else {
            usage_error("unknown flag: " + std::string(arg));
        }
    }
    options.min_iterations = std::min(options.min_iterations, options.iterations);
    return options;
}

std::string action_label(const TreeAction& action) {
    const int pct = static_cast<int>(std::lround(action.pot_fraction * 100.0));
    switch (action.type) {
        case ActionType::Fold: return "F";
        case ActionType::Check: return "X";
        case ActionType::Call: return "C";
        case ActionType::Bet: return "B" + std::to_string(pct);
        case ActionType::Raise: return "R" + std::to_string(pct);
        case ActionType::AllIn: return "A";
    }
    return "?";
}

std::string combo_text(int combo) {
    const std::array<Card, 2> cards = combo_cards(combo);
    return cards[1].to_string() + cards[0].to_string();
}

bool aggressive(ActionType type) {
    return type == ActionType::Bet || type == ActionType::Raise || type == ActionType::AllIn;
}

// The root action the advisor's action stands for, and whether the match is exact
// (same type and street total). A bet or raise with no exact match takes the
// aggressive root action nearest in pot fraction.
std::pair<std::size_t, bool> map_action(const Action& advice, const PlayerView& view,
                                        const std::vector<TreeAction>& actions) {
    const auto find_type = [&](ActionType type) -> std::optional<std::size_t> {
        for (std::size_t a = 0; a < actions.size(); ++a) {
            if (actions[a].type == type) return a;
        }
        return std::nullopt;
    };
    switch (advice.type) {
        case ActionType::Fold:
        case ActionType::Check:
        case ActionType::Call:
            if (const auto a = find_type(advice.type)) return {*a, true};
            break;
        case ActionType::AllIn:
            if (const auto a = find_type(ActionType::AllIn)) return {*a, true};
            if (const auto a = find_type(ActionType::Call)) return {*a, false};  // An all-in that only calls
            break;
        case ActionType::Bet:
        case ActionType::Raise: {
            for (std::size_t a = 0; a < actions.size(); ++a) {
                if (aggressive(actions[a].type) && actions[a].to == advice.amount) return {a, true};
            }
            const int facing = std::max(0, view.current_bet - view.seats[static_cast<std::size_t>(view.seat)].street_bet);
            const double fraction =
                static_cast<double>(advice.amount - view.current_bet) / static_cast<double>(view.total_pot + facing);
            std::optional<std::size_t> nearest;
            for (std::size_t a = 0; a < actions.size(); ++a) {
                if (!aggressive(actions[a].type)) continue;
                if (!nearest ||
                    std::abs(actions[a].pot_fraction - fraction) < std::abs(actions[*nearest].pot_fraction - fraction)) {
                    nearest = a;
                }
            }
            if (nearest) return {*nearest, false};
            break;
        }
    }
    throw std::runtime_error("advisor action " + std::string(to_string(advice.type)) + " has no root action");
}

struct ComboResult {
    int combo = 0;
    double share = 0.0;  // Renormalised over the sample
    std::size_t advisor = 0;
    bool exact = true;
    std::size_t top = 0;
    double regret_bb = 0.0;
    double freq = 0.0;
    double ev_advisor_bb = 0.0;
    double ev_max_bb = 0.0;
    std::string best_action;
    double millis = 0.0;
};

struct SpotMetrics {
    double regret = 0.0;     // bb per decision
    double regret_pot = 0.0;  // The same as % of the pot at the decision
    double over_half = 0.0;  // Share with regret > 0.5 bb
    double over_one = 0.0;
    double agree = 0.0;
    double freq = 0.0;
    double rare = 0.0;  // Advisor action used < 10% by the solver
};

struct SpotResult {
    const bench::BenchSpot* spot = nullptr;
    int hands = 0;
    int iterations = 0;
    double exploitability = 0.0;
    double solve_ms = 0.0;
    int inexact = 0;
    std::vector<std::string> labels;
    std::vector<double> solver_mix;   // Share-weighted solver frequencies over the sample
    std::vector<double> advisor_mix;  // Share-weighted advisor choices
    std::vector<ComboResult> combos;
    SpotMetrics metrics;
    // --stability: at twice the iterations, the share-weighted mean |change| in the
    // sampled combos' regret, and the change in the spot's mean regret (bb)
    int stability_iterations = 0;
    double stability_abs_change = 0.0;
    double stability_mean_change = 0.0;
};

// Up to `count` hands spread evenly over the root player's hands sorted by river
// strength (strongest first, ties by combo index): a systematic sample that keeps
// value hands, bluff-catchers and air in proportion.
std::vector<std::size_t> sample_hands(const RootSolution& root, const std::vector<Card>& board, int count) {
    std::vector<std::size_t> live;
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        if (root.share[h] > 0.0) live.push_back(h);
    }
    const std::array<Card, 5> five{board[0], board[1], board[2], board[3], board[4]};
    std::vector<HandRank> rank(root.hands.size());
    for (const std::size_t h : live) rank[h] = evaluate7(combo_cards(root.hands[h]), five);
    std::sort(live.begin(), live.end(), [&](std::size_t a, std::size_t b) {
        if (rank[a] != rank[b]) return rank[a] < rank[b];
        return root.hands[a] < root.hands[b];
    });
    const auto n = live.size();
    const auto k = static_cast<std::size_t>(count);
    if (n <= k) return live;
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < k; ++i) picked.push_back(live[(2 * i + 1) * n / (2 * k)]);
    return picked;
}

SpotResult run_spot(const bench::BenchFile& file, const bench::BenchSpot& spot, const Options& options) {
    SpotResult result;
    result.spot = &spot;
    const double bb = file.big_blind;
    const CardMask board = card_mask(std::span<const Card>(spot.board));
    const std::size_t hero = static_cast<std::size_t>(spot.hero());

    // Any legal deal gives the same public state: use the first combos of each range.
    const std::optional<int> hero_combo = bench::first_combo(spot.ranges[hero], board);
    if (!hero_combo) throw std::runtime_error(spot.id + ": empty hero range");
    const auto villain_for = [&](int combo) {
        const std::optional<int> villain = bench::first_combo(spot.ranges[1 - hero], board | combo_mask(combo));
        if (!villain) throw std::runtime_error(spot.id + ": no villain combo fits " + combo_text(combo));
        return combo_cards(*villain);
    };
    const GameEngine first =
        bench::replay_spot(file, spot, combo_cards(*hero_combo), villain_for(*hero_combo));
    const PlayerView first_view = first.player_view(bench::hero_seat(spot));

    const auto start = std::chrono::steady_clock::now();
    const SolveSpot solve_spot = spot_from_view(first_view, spot.ranges[hero], spot.ranges[1 - hero]);
    BetAbstraction abstraction = river_default_abstraction();
    abstraction.all_in_merge = 1.0;
    PostflopSolver solver(solve_spot, abstraction);
    double exploitability = 0.0;
    while (solver.iterations() < options.iterations) {
        solver.run(std::min(kCheckEvery, options.iterations - solver.iterations()));
        exploitability = solver.exploitability_pct_pot();
        if (exploitability <= options.target && solver.iterations() >= options.min_iterations) break;
    }
    const RootSolution root = solver.root();
    result.solve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.iterations = solver.iterations();
    result.exploitability = exploitability;
    if (static_cast<std::size_t>(root.player) != hero) throw std::runtime_error(spot.id + ": root player is not the hero");

    const std::size_t actions = root.actions.size();
    for (const TreeAction& action : root.actions) result.labels.push_back(action_label(action));
    result.solver_mix.assign(actions, 0.0);
    result.advisor_mix.assign(actions, 0.0);

    const std::vector<std::size_t> sample = sample_hands(root, spot.board, options.combos);
    for (std::size_t h = 0; h < root.hands.size(); ++h) result.hands += root.share[h] > 0.0 ? 1 : 0;
    double mass = 0.0;
    for (const std::size_t h : sample) mass += root.share[h];

    const TrainingAdvisor advisor(options.seed, options.rollouts);
    for (const std::size_t h : sample) {
        const int combo = root.hands[h];
        const GameEngine engine = bench::replay_spot(file, spot, combo_cards(combo), villain_for(combo));
        const PlayerView view = engine.player_view(bench::hero_seat(spot));
        const auto asked = std::chrono::steady_clock::now();
        const HintRecommendation hint = advisor.analyze(view);
        const double millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();

        ComboResult row;
        row.combo = combo;
        row.share = root.share[h] / mass;
        const auto [advised, exact] = map_action(hint.action, view, root.actions);
        row.advisor = advised;
        row.exact = exact;
        double best_ev = root.ev_best[0][h];
        for (std::size_t a = 1; a < actions; ++a) best_ev = std::max(best_ev, static_cast<double>(root.ev_best[a][h]));
        for (std::size_t a = 1; a < actions; ++a) {
            if (root.freq[a][h] > root.freq[row.top][h]) row.top = a;
        }
        row.ev_advisor_bb = root.ev_best[advised][h] / bb;
        row.ev_max_bb = best_ev / bb;
        row.regret_bb = std::max(0.0, best_ev - root.ev_best[advised][h]) / bb;
        row.freq = root.freq[advised][h];
        row.best_action = hint.best_action;
        row.millis = millis;
        result.inexact += exact ? 0 : 1;
        for (std::size_t a = 0; a < actions; ++a) result.solver_mix[a] += row.share * root.freq[a][h];
        result.advisor_mix[advised] += row.share;

        SpotMetrics& m = result.metrics;
        m.regret += row.share * row.regret_bb;
        m.regret_pot += row.share * row.regret_bb * bb * 100.0 / spot.pot;
        m.over_half += row.regret_bb > 0.5 ? row.share : 0.0;
        m.over_one += row.regret_bb > 1.0 ? row.share : 0.0;
        m.agree += advised == row.top ? row.share : 0.0;
        m.freq += row.share * row.freq;
        m.rare += row.freq < 0.10 ? row.share : 0.0;
        result.combos.push_back(row);
    }

    if (options.stability) {
        result.stability_iterations = 2 * result.iterations;
        solver.run(result.stability_iterations - solver.iterations());
        const RootSolution again = solver.root();
        double regret = 0.0;
        for (std::size_t i = 0; i < sample.size(); ++i) {
            const std::size_t h = sample[i];
            const ComboResult& row = result.combos[i];
            double best_ev = again.ev_best[0][h];
            for (std::size_t a = 1; a < actions; ++a) best_ev = std::max(best_ev, static_cast<double>(again.ev_best[a][h]));
            const double regret_bb = std::max(0.0, best_ev - again.ev_best[row.advisor][h]) / bb;
            result.stability_abs_change += row.share * std::abs(regret_bb - row.regret_bb);
            regret += row.share * regret_bb;
        }
        result.stability_mean_change = regret - result.metrics.regret;
    }
    return result;
}

std::string pct(double share) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f%%", 100.0 * share);
    return text;
}

std::string fixed(double value, int digits) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", digits, value);
    return text;
}

std::string mix_text(const std::vector<std::string>& labels, const std::vector<double>& mix) {
    std::string text;
    for (std::size_t a = 0; a < labels.size(); ++a) {
        if (mix[a] < 0.005) continue;
        if (!text.empty()) text += " ";
        text += labels[a] + " " + pct(mix[a]);
    }
    return text;
}

void print_group(const std::string& title, const std::vector<SpotResult>& results,
                 std::string (*key)(const SpotResult&)) {
    std::map<std::string, std::vector<const SpotResult*>> groups;
    std::vector<std::string> order;
    for (const SpotResult& result : results) {
        const std::string name = key(result);
        if (groups.find(name) == groups.end()) order.push_back(name);
        groups[name].push_back(&result);
    }
    std::cout << "\n### By " << title << "\n\n"
              << "| " << title << " | spots | regret bb | regret % pot | regret > 0.5 bb | regret > 1 bb | agree | solver freq of advice"
              << " | advice used < 10% |\n|---|---|---|---|---|---|---|---|---|\n";
    for (const std::string& name : order) {
        SpotMetrics sum;
        const auto& members = groups[name];
        for (const SpotResult* r : members) {
            sum.regret += r->metrics.regret;
            sum.regret_pot += r->metrics.regret_pot;
            sum.over_half += r->metrics.over_half;
            sum.over_one += r->metrics.over_one;
            sum.agree += r->metrics.agree;
            sum.freq += r->metrics.freq;
            sum.rare += r->metrics.rare;
        }
        const double n = static_cast<double>(members.size());
        std::cout << "| " << name << " | " << members.size() << " | " << fixed(sum.regret / n, 3) << " | " << fixed(sum.regret_pot / n, 2) << " | "
                  << pct(sum.over_half / n) << " | " << pct(sum.over_one / n) << " | " << pct(sum.agree / n) << " | "
                  << pct(sum.freq / n) << " | " << pct(sum.rare / n) << " |\n";
    }
}

double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(q * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    const auto started = std::chrono::steady_clock::now();
    bench::BenchFile file;
    try {
        file = bench::load_bench_file(options.spots);
    } catch (const std::exception& error) {
        std::cerr << "advisor_bench: " << error.what() << '\n';
        return 2;
    }

    std::vector<SpotResult> results;
    try {
        for (const bench::BenchSpot& spot : file.spots) {
            if (options.only && spot.id != *options.only) continue;
            results.push_back(run_spot(file, spot, options));
            std::cerr << "solved " << spot.id << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "advisor_bench: " << error.what() << '\n';
        return 1;
    }
    if (results.empty()) {
        std::cerr << "advisor_bench: no spot matches\n";
        return 2;
    }

    std::cout << "## Advisor vs river solver\n\n"
              << "Solver: target " << options.target << "% of pot, " << options.min_iterations << " to "
              << options.iterations << " iterations. Advisor: seed " << options.seed << ", " << options.rollouts
              << " rollouts. Up to " << options.combos << " hero combos per spot, weighted by root share.\n"
              << "Regret uses best-continuation EVs (ev_best). Ranges are assumed, not solved.\n\n"
              << "| spot | hero | hands | sampled | iters | expl % pot | regret bb | > 0.5 bb | > 1 bb | agree | freq"
              << " | < 10% | inexact maps |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    bool all_converged = true;
    for (const SpotResult& r : results) {
        all_converged = all_converged && r.exploitability <= options.target;
        const SpotMetrics& m = r.metrics;
        std::cout << "| " << r.spot->id << " | " << r.spot->role() << " | " << r.hands << " | " << r.combos.size()
                  << " | " << r.iterations << " | " << fixed(r.exploitability, 3) << " | " << fixed(m.regret, 3)
                  << " | " << pct(m.over_half) << " | " << pct(m.over_one) << " | " << pct(m.agree) << " | "
                  << pct(m.freq) << " | " << pct(m.rare) << " | " << r.inexact << " |\n";
    }

    std::cout << "\n### Action mix (share-weighted over the sample)\n\n| spot | solver | advisor |\n|---|---|---|\n";
    for (const SpotResult& r : results) {
        std::cout << "| " << r.spot->id << " | " << mix_text(r.labels, r.solver_mix) << " | "
                  << mix_text(r.labels, r.advisor_mix) << " |\n";
    }

    print_group("pot type", results, [](const SpotResult& r) { return r.spot->pot_type; });
    print_group("board family", results, [](const SpotResult& r) { return r.spot->family; });
    print_group("line", results, [](const SpotResult& r) { return r.spot->line; });
    print_group("hero role", results, [](const SpotResult& r) { return r.spot->role(); });
    print_group("all", results, [](const SpotResult&) { return std::string("overall"); });

    double solve_ms = 0.0;
    std::vector<double> millis;
    for (const SpotResult& r : results) {
        solve_ms += r.solve_ms;
        for (const ComboResult& c : r.combos) millis.push_back(c.millis);
    }
    std::cout << "\nSolver time " << fixed(solve_ms / 1000.0, 1) << " s over " << results.size() << " spots; "
              << (all_converged ? "every spot reached the target.\n" : "SOME SPOTS MISSED THE TARGET.\n");
    if (options.stability) {
        std::cout << "\n### Stability (re-solved at twice the iterations)\n\n"
                  << "| spot | iters | mean abs combo regret change bb | spot regret change bb |\n|---|---|---|---|\n";
        double abs_sum = 0.0;
        double mean_sum = 0.0;
        double max_abs = 0.0;
        for (const SpotResult& r : results) {
            std::cout << "| " << r.spot->id << " | " << r.stability_iterations << " | " << fixed(r.stability_abs_change, 4)
                      << " | " << fixed(r.stability_mean_change, 4) << " |\n";
            abs_sum += r.stability_abs_change;
            mean_sum += r.stability_mean_change;
            max_abs = std::max(max_abs, std::abs(r.stability_mean_change));
        }
        const double n = static_cast<double>(results.size());
        std::cout << "\nStability over " << results.size() << " spots: mean abs combo regret change "
                  << fixed(abs_sum / n, 4) << " bb; overall regret change " << fixed(mean_sum / n, 4)
                  << " bb; largest spot change " << fixed(max_abs, 4) << " bb.\n";
    }
    if (options.timing) {
        std::cout << "Advisor ms per decision: p50 " << fixed(percentile(millis, 0.5), 2) << ", p95 "
                  << fixed(percentile(millis, 0.95), 2) << " over " << millis.size() << " decisions.\n";
    }

    if (options.csv) {
        std::ofstream csv(*options.csv);
        if (!csv) {
            std::cerr << "advisor_bench: cannot write " << *options.csv << '\n';
            return 2;
        }
        csv << "spot,combo,class,share,advisor_best_action,advisor_root_action,exact_map,solver_top_action,"
               "solver_freq_of_advice,ev_best_advice_bb,ev_best_max_bb,regret_bb,agree,advisor_ms\n";
        for (const SpotResult& r : results) {
            for (const ComboResult& c : r.combos) {
                csv << r.spot->id << ',' << combo_text(c.combo) << ',' << combo_class(c.combo).to_string() << ','
                    << fixed(c.share, 6) << ",\"" << c.best_action << "\"," << r.labels[c.advisor] << ','
                    << (c.exact ? 1 : 0) << ',' << r.labels[c.top] << ',' << fixed(c.freq, 4) << ','
                    << fixed(c.ev_advisor_bb, 3) << ',' << fixed(c.ev_max_bb, 3) << ',' << fixed(c.regret_bb, 3)
                    << ',' << (c.advisor == c.top ? 1 : 0) << ',' << fixed(c.millis, 2) << '\n';
            }
        }
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "Total runtime " << fixed(total, 1) << " s.\n";

    int inexact = 0;
    for (const SpotResult& r : results) inexact += r.inexact;
    if (inexact > 0) {
        std::cout << "\nWARNING: " << inexact << " advisor action(s) mapped inexactly to a root action; regret for"
                  << " those combos is measured on the nearest root size. See the \"inexact maps\" column.\n";
        std::cerr << "advisor_bench: WARNING: " << inexact << " inexact advisor action map(s)\n";
        return 3;
    }
    return 0;
}
