// Solves one heads-up postflop spot with the DCFR solver and prints the root
// strategy by hand class. For people checking spots and as a smoke test.
//
//   solver_cli --board Kh9d5c3s2h --pot 300 --stack 1000 --oop "RANGE" --ip "RANGE"
//              [--bets B0,B1] [--to-act oop|ip] [--iterations N] [--target PCT]
//              [--sizes 25,50,75,100] [--big-blind N] [--no-timing] [--verbose]
//
// Ranges use HandRange syntax with optional per-token weights ("AKo:0.5").

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "range/combos.h"
#include "range/weighted_range.h"
#include "solver/betting_tree.h"
#include "solver/postflop_solver.h"
#include "range_text.h"

using namespace poker;

namespace {

constexpr int kCheckEvery = 25;  // Iterations between exploitability checks

constexpr std::string_view kUsage =
    "Usage: solver_cli --board CARDS --pot N --stack N --oop RANGE --ip RANGE\n"
    "                  [--bets B0,B1] [--to-act oop|ip] [--iterations N] [--target PCT]\n"
    "                  [--sizes 25,50,75,100] [--big-blind N] [--no-timing] [--verbose]\n"
    "  --board       3-5 cards, e.g. Kh9d5c3s2h (5 = river, 4 = turn, 3 = flop)\n"
    "  --pot         chips in the middle from earlier streets\n"
    "  --stack       effective stack behind once the larger street bet is matched\n"
    "  --bets        street bets already in, OOP,IP (default 0,0)\n"
    "  --to-act      who acts at the root (default oop; ip with equal bets means OOP checked)\n"
    "  --oop, --ip   HandRange syntax, tokens may end in :weight, e.g. \"QQ+, AKs, AKo:0.5\"\n"
    "  --iterations  maximum iterations (default 1000)\n"
    "  --target      stop once exploitability is at most PCT % of pot (checked every 25)\n"
    "  --sizes       root bet (or raise) sizes in % of pot (default 25,50,75,100)\n"
    "  --big-blind   smallest bet and raise increment (default 20)\n";

struct Options {
    std::vector<Card> board;
    int pot = -1;
    int stack = -1;
    std::array<int, 2> bets{};
    int to_act = 0;
    std::array<std::optional<std::string>, 2> ranges;
    int iterations = 1000;
    std::optional<double> target;
    std::optional<std::vector<double>> sizes;
    int big_blind = 20;
    bool timing = true;
    bool verbose = false;
};

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << "solver_cli: " << message << "\n\n" << kUsage;
    std::exit(2);
}

[[noreturn]] void spot_error(const std::string& message) {
    std::cerr << "solver_cli: " << message << '\n';
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
        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (arg == "--no-timing") {
            options.timing = false;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            std::exit(0);
        }
        if (i + 1 >= argc) usage_error("unknown flag or missing value: " + std::string(arg));
        const std::string_view value = argv[++i];
        try {
            if (arg == "--board") {
                options.board = tools::parse_board(value);
            } else if (arg == "--pot") {
                options.pot = int_flag(arg, value, 1);
            } else if (arg == "--stack") {
                options.stack = int_flag(arg, value, 0);
            } else if (arg == "--bets") {
                const std::vector<double> bets = tools::parse_number_list(value);
                if (bets.size() != 2) usage_error("--bets needs two numbers, OOP,IP");
                for (std::size_t p = 0; p < 2; ++p) {
                    if (bets[p] < 0.0 || bets[p] != std::floor(bets[p]) || bets[p] > 1e9) {
                        usage_error("--bets needs whole, non-negative chips");
                    }
                    options.bets[p] = static_cast<int>(bets[p]);
                }
            } else if (arg == "--to-act") {
                if (value == "oop") {
                    options.to_act = 0;
                } else if (value == "ip") {
                    options.to_act = 1;
                } else {
                    usage_error("--to-act must be oop or ip");
                }
            } else if (arg == "--oop" || arg == "--ip") {
                options.ranges[arg == "--oop" ? 0u : 1u] = std::string(value);
            } else if (arg == "--iterations") {
                options.iterations = int_flag(arg, value, 1);
            } else if (arg == "--target") {
                const std::optional<double> target = tools::parse_double(value);
                if (!target || *target < 0.0) usage_error("--target needs a non-negative percentage");
                options.target = *target;
            } else if (arg == "--sizes") {
                std::vector<double> sizes = tools::parse_number_list(value);
                for (double& size : sizes) {
                    if (size <= 0.0) usage_error("--sizes must all be positive");
                    size /= 100.0;
                }
                options.sizes = sizes;
            } else if (arg == "--big-blind") {
                options.big_blind = int_flag(arg, value, 1);
            } else {
                usage_error("unknown flag: " + std::string(arg));
            }
        } catch (const std::invalid_argument& error) {
            usage_error(std::string(arg) + ": " + error.what());
        }
    }
    if (options.board.empty()) usage_error("--board is required");
    if (options.pot < 0) usage_error("--pot is required");
    if (options.stack < 0) usage_error("--stack is required");
    if (!options.ranges[0] || !options.ranges[1]) usage_error("--oop and --ip are required");
    return options;
}

// The river abstraction. On a turn or flop board the earlier streets get the
// deeper river sizes too (33/75/150% + all-in, raises pot + all-in), so the root
// has the same options whatever the street; the memory estimate is printed first.
BetAbstraction abstraction_for(const Options& options) {
    BetAbstraction abstraction = river_default_abstraction();
    for (std::size_t street = 0; street < 2; ++street) abstraction.streets[street] = abstraction.streets[2];
    if (options.sizes) abstraction.root_sizes = *options.sizes;
    return abstraction;
}

std::string board_text(const std::vector<Card>& board) {
    std::string text;
    for (const Card card : board) {
        if (!text.empty()) text += ' ';
        text += card.to_string();
    }
    return text;
}

std::string action_label(const TreeAction& action) {
    const int percent = static_cast<int>(std::lround(action.pot_fraction * 100.0));
    switch (action.type) {
        case ActionType::Fold: return "F";
        case ActionType::Check: return "X";
        case ActionType::Call: return "C";
        case ActionType::Bet: return "B" + std::to_string(percent);
        case ActionType::Raise: return "R" + std::to_string(percent);
        case ActionType::AllIn: return "A";
    }
    return "?";
}

std::string fixed(double value, int decimals) {
    if (std::fabs(value) < 0.5 * std::pow(10.0, -decimals)) value = 0.0;  // No "-0.0"
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

std::string pad(std::string text, std::size_t width) {
    if (text.size() < width) text.insert(0, width - text.size(), ' ');
    return text;
}

std::string bytes_text(std::size_t bytes) {
    if (bytes < 10 * 1024) return std::to_string(bytes) + " B";
    if (bytes < 10 * 1024 * 1024) return fixed(static_cast<double>(bytes) / 1024.0, 1) + " KB";
    return fixed(static_cast<double>(bytes) / (1024.0 * 1024.0), 1) + " MB";
}

// Hands dealt to a player once the board is removed, and their summed weight.
struct RangeSize {
    int combos = 0;
    double weight = 0.0;
};

RangeSize range_size(const WeightedRange& range, CardMask board) {
    RangeSize size;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (range.weight(combo) <= 0.0f || (combo_mask(combo) & board) != 0) continue;
        ++size.combos;
        size.weight += range.weight(combo);
    }
    return size;
}

void print_strategy_header(std::string_view first, const std::vector<std::string>& labels) {
    std::cout << first << "  share |";
    for (const std::string& label : labels) std::cout << pad(label, 6);
    std::cout << " | EV:";
    for (const std::string& label : labels) std::cout << pad(label, 7);
    std::cout << '\n';
}

void print_strategy_row(std::string_view name, double share, const std::vector<double>& freq,
                        const std::vector<double>& ev) {
    std::cout << name << pad(fixed(100.0 * share, 2), 7) << " |";
    for (const double f : freq) std::cout << pad(fixed(100.0 * f, 1), 6);
    std::cout << " |    ";
    for (const double e : ev) std::cout << pad(fixed(e, 1), 7);
    std::cout << '\n';
}

void print_root(const RootSolution& root, bool verbose) {
    const std::size_t actions = root.actions.size();
    std::vector<std::string> labels;
    for (const TreeAction& action : root.actions) labels.push_back(action_label(action));

    std::cout << "\nRoot actions (" << (root.player == 0 ? "OOP" : "IP") << " to act):";
    for (std::size_t a = 0; a < actions; ++a) {
        std::cout << ' ' << labels[a];
        if (root.actions[a].adds > 0) std::cout << "(to " << root.actions[a].to << ')';
    }
    std::cout << '\n';

    // Share-weighted sums per class, in preflop strength order.
    struct ClassSums {
        double share = 0.0;
        std::vector<double> freq;
        std::vector<double> ev;
    };
    std::vector<ClassSums> classes(kHandClassCount, ClassSums{0.0, std::vector<double>(actions), std::vector<double>(actions)});
    std::vector<double> overall(actions);
    double overall_ev = 0.0;
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        const double share = root.share[h];
        ClassSums& sums = classes[static_cast<std::size_t>(combo_class(root.hands[h]).index())];
        sums.share += share;
        for (std::size_t a = 0; a < actions; ++a) {
            sums.freq[a] += share * root.freq[a][h];
            sums.ev[a] += share * root.ev[a][h];
            overall[a] += share * root.freq[a][h];
        }
        overall_ev += share * root.hand_ev[h];
    }

    std::cout << "Overall:";
    for (std::size_t a = 0; a < actions; ++a) std::cout << ' ' << labels[a] << ' ' << fixed(100.0 * overall[a], 1) << '%';
    std::cout << " | EV " << fixed(overall_ev, 1) << " chips\n\n";

    print_strategy_header("class", labels);
    for (const HandClass hand : hand_classes_by_strength()) {
        const ClassSums& sums = classes[static_cast<std::size_t>(hand.index())];
        if (sums.share <= 0.0) continue;
        std::vector<double> freq(actions);
        std::vector<double> ev(actions);
        for (std::size_t a = 0; a < actions; ++a) {
            freq[a] = sums.freq[a] / sums.share;
            ev[a] = sums.ev[a] / sums.share;
        }
        std::string name = hand.to_string();
        name.resize(5, ' ');
        print_strategy_row(name, sums.share, freq, ev);
    }

    if (!verbose) return;
    std::cout << '\n';
    print_strategy_header("combo", labels);
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        std::vector<double> freq(actions);
        std::vector<double> ev(actions);
        for (std::size_t a = 0; a < actions; ++a) {
            freq[a] = root.freq[a][h];
            ev[a] = root.ev[a][h];
        }
        const std::array<Card, 2> cards = combo_cards(root.hands[h]);
        std::string name = cards[1].to_string() + cards[0].to_string();
        name.resize(5, ' ');
        print_strategy_row(name, root.share[h], freq, ev);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    SolveSpot spot;
    spot.board = options.board;
    spot.pot = options.pot;
    spot.effective_stack = options.stack;
    spot.street_bet = options.bets;
    spot.to_act = options.to_act;
    spot.min_bet = options.big_blind;
    const CardMask board_mask = card_mask(options.board);
    std::array<RangeSize, 2> sizes{};
    for (std::size_t p = 0; p < 2; ++p) {
        const char* name = p == 0 ? "--oop" : "--ip";
        try {
            spot.ranges[p] = tools::parse_weighted_range(*options.ranges[p]);
        } catch (const std::invalid_argument& error) {
            usage_error(std::string(name) + ": " + error.what());
        }
        sizes[p] = range_size(spot.ranges[p], board_mask);
        if (sizes[p].combos == 0) spot_error(std::string(name) + " range has no combos left once the board is removed");
    }
    const BetAbstraction abstraction = abstraction_for(options);

    static constexpr std::array<std::string_view, 3> kStreetNames{"flop", "turn", "river"};
    std::cout << "Spot: " << board_text(options.board) << " (" << kStreetNames[options.board.size() - 3]
              << "), pot " << options.pot << ", stack " << options.stack << ", bets " << options.bets[0] << '/'
              << options.bets[1] << ", " << (options.to_act == 0 ? "OOP" : "IP") << " to act, big blind "
              << options.big_blind << '\n';
    for (std::size_t p = 0; p < 2; ++p) {
        std::cout << (p == 0 ? "OOP: " : "IP:  ") << sizes[p].combos << " combos, weight " << fixed(sizes[p].weight, 1)
                  << '\n';
    }

    std::optional<PostflopSolver> solver;
    try {
        std::cout << "Estimated memory: " << bytes_text(PostflopSolver::estimate_bytes(spot, abstraction)) << '\n';
        solver.emplace(spot, abstraction);
    } catch (const SolverMemoryError& error) {
        spot_error(error.what());
    } catch (const std::invalid_argument& error) {
        spot_error(std::string("invalid spot: ") + error.what());
    }

    const BettingTree& tree = solver->tree();
    std::cout << "Tree: " << tree.nodes.size() << " nodes, " << tree.decision_nodes << " decision, "
              << tree.decision_actions << " slots, " << bytes_text(tree.bytes()) << "; solver "
              << bytes_text(solver->allocated_bytes()) << '\n';

    std::cout << "\n  iter  expl %pot" << (options.timing ? "       ms" : "") << '\n';
    const auto start = std::chrono::steady_clock::now();
    double exploitability = 0.0;
    while (solver->iterations() < options.iterations) {
        solver->run(std::min(kCheckEvery, options.iterations - solver->iterations()));
        exploitability = solver->exploitability_pct_pot();
        std::cout << pad(std::to_string(solver->iterations()), 6) << pad(fixed(exploitability, 4), 11);
        if (options.timing) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cout << pad(std::to_string(elapsed.count()), 9);
        }
        std::cout << '\n';
        if (options.target && exploitability <= *options.target) break;
    }
    std::cout << "Result: " << solver->iterations() << " iterations, exploitability " << fixed(exploitability, 4)
              << "% of pot";
    if (options.target) std::cout << (exploitability <= *options.target ? " (target met)" : " (target not met)");
    std::cout << '\n';

    print_root(solver->root(), options.verbose);
    return 0;
}
