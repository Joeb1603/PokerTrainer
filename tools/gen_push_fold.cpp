// Generates src/strategy/generated/push_fold_data.cpp: push/fold ranges for
// effective stacks of 3 to 15 BB, blinds 0.5/1 BB, no ante; and
// src/strategy/generated/push_fold_ante_data.cpp: the same with dead money from
// antes (detail::kPushFoldAnteDeadBb, in BB, for the whole table).
//
//   gen_push_fold [--iterations N] [--threads N] [--out PATH] [--ante-out PATH] [--verbose]
//
// Antes: every ante, the player's own included, is dead money D already in the
// pot; stacks are measured after the antes. D joins every pot: the showdown pot
// and the blinds a jam picks up when everybody folds. Nothing else changes.
//
// Method: fictitious play over the 169 hand classes, one game per first-in
// position and stack. Each player's strategy is a per-class probability. Every
// iteration the jammer best-responds to the callers' average strategies, its
// average moves 1/(t+1) of the way to that response, and then each caller does
// the same against the jammer's updated average. The published sets are the
// classes whose final average is at least 0.5.
//
// The games:
// - SB first in: the SB jams or folds and the BB calls or folds. This is also the
//   heads-up game (the button posts the small blind).
// - UTG, HJ, CO or BTN first in: every player behind is a possible caller. The
//   first to call is the only one (overcalls are ignored), so the jammer's value
//   is sum over callers m of P(all before m fold) * (its showdown against m's
//   calling range), plus P(all fold) * the blinds. Each caller's range is its best
//   response heads-up against the jam, as if it were the one caller; whether the
//   players behind it would also call is ignored.
// - Card removal: the chance that a caller holds class j given the jammer holds i
//   is the number of disjoint combo pairs of (i, j) over all pairs, which is exact
//   for two players. Between callers, and for the folded players, it is ignored.
// - Equity is class_vs_class_equity (all-in preflop, 200,000 rollouts per pair).
//
// Everything is deterministic: no randomness, a fixed order for every sum, and
// a strict "better than folding" test for each best response. Jobs are split
// over threads by game, and each writes only its own results.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "range/combos.h"
#include "range/preflop_matrix.h"
#include "strategy/preflop_charts.h"
#include "strategy/push_fold.h"

using namespace poker;

namespace {

constexpr int kDefaultIterations = 2000;
constexpr std::size_t kClasses = kHandClassCount;
constexpr int kJammers = detail::kPushFoldJammerCount;
constexpr int kPositions = detail::kPushFoldPositionCount;
constexpr int kStacks = detail::kPushFoldStackCount;

struct Options {
    int iterations = kDefaultIterations;
    int threads = 0;
    bool verbose = false;  // per-game convergence
    std::string out = "src/strategy/generated/push_fold_data.cpp";
    std::string ante_out = "src/strategy/generated/push_fold_ante_data.cpp";
};

using ClassVector = std::array<double, kClasses>;
using ClassMatrix = std::array<ClassVector, kClasses>;

// Shared inputs, by HandClass::index().
struct Inputs {
    ClassMatrix given{};   // given[i][j]: P(opponent holds j | we hold i), card removal included
    ClassMatrix equity{};  // equity[i][j]: all-in equity of i against j
    ClassVector prior{};   // combos / 1326
    std::array<int, kClasses> combos{};
};

// On the heap: two 169 x 169 matrices are too big for a comfortable stack.
std::unique_ptr<const Inputs> build_inputs() {
    auto owned = std::make_unique<Inputs>();
    Inputs& in = *owned;
    std::array<HandClass, kClasses> by_index{};
    for (const HandClass hand : hand_classes_by_strength()) by_index[static_cast<std::size_t>(hand.index())] = hand;
    for (std::size_t i = 0; i < kClasses; ++i) {
        in.combos[i] = by_index[i].combos();
        in.prior[i] = static_cast<double>(in.combos[i]) / kTotalCombos;
    }
    // Each combo leaves C(50, 2) = 1225 two-card hands, so each row sums to 1.
    constexpr int kHandsLeft = 1225;
    for (std::size_t i = 0; i < kClasses; ++i) {
        const ClassCombos& mine = class_combos(by_index[i]);
        for (std::size_t j = 0; j < kClasses; ++j) {
            const ClassCombos& theirs = class_combos(by_index[j]);
            int disjoint = 0;
            for (const std::uint16_t a : mine.view()) {
                for (const std::uint16_t b : theirs.view()) {
                    if ((combo_mask(a) & combo_mask(b)) == 0) ++disjoint;
                }
            }
            in.given[i][j] = static_cast<double>(disjoint) / (static_cast<double>(in.combos[i]) * kHandsLeft);
            in.equity[i][j] = class_vs_class_equity(by_index[i], by_index[j]);
        }
    }
    return owned;
}

double blind_of(int position) {
    if (position == static_cast<int>(Position::SB)) return 0.5;
    if (position == static_cast<int>(Position::BB)) return 1.0;
    return 0.0;
}

struct Result {
    ClassVector jam{};
    std::array<ClassVector, kPositions> call{};  // by caller position
    int last_change = 0;                         // last iteration a published set changed
    double nash_conv_mixed = 0.0;                // BB per hand, summed over players
    double nash_conv_pure = 0.0;
};

class Game {
public:
    Game(const Inputs& in, int jammer, int stack_bb, double dead_bb)
        : in_(in), jammer_(jammer), stack_(stack_bb), dead_(dead_bb) {
        for (int c = jammer + 1; c < kPositions; ++c) callers_.push_back(c);
    }

    // The jammer's value of jamming each class, against the callers' strategies.
    ClassVector jam_values(const std::array<ClassVector, kPositions>& call) const {
        ClassVector values{};
        const double own_blind = blind_of(jammer_);
        for (std::size_t i = 0; i < kClasses; ++i) {
            double reach = 1.0;  // everybody before this caller folded
            double value = 0.0;
            for (const int c : callers_) {
                const ClassVector& calls = call[static_cast<std::size_t>(c)];
                double called = 0.0;
                double won = 0.0;
                for (std::size_t j = 0; j < kClasses; ++j) {
                    const double w = in_.given[i][j] * calls[j];
                    called += w;
                    won += w * in_.equity[i][j];
                }
                value += reach * (won * pot(c) - called * stack_);
                reach *= 1.0 - called;
            }
            values[i] = value + reach * (1.5 + dead_ - own_blind);
        }
        return values;
    }

    // A caller's value of calling each class (minus folding's), against the jam.
    // Classes the jammer never meets get -1, a fold.
    ClassVector call_gains(int caller, const ClassVector& jam) const {
        ClassVector gains{};
        for (std::size_t j = 0; j < kClasses; ++j) {
            double jammed = 0.0;
            double won = 0.0;
            for (std::size_t i = 0; i < kClasses; ++i) {
                const double w = in_.given[j][i] * jam[i];
                jammed += w;
                won += w * in_.equity[j][i];
            }
            gains[j] = jammed > 0.0 ? won / jammed * pot(caller) - stack_ + blind_of(caller) : -1.0;
        }
        return gains;
    }

    double fold_value() const { return -blind_of(jammer_); }

    // Sum over players of what a best response gains on the profile, in BB per
    // hand dealt to that player (each caller's term is conditional on facing a
    // lone jam).
    double nash_conv(const ClassVector& jam, const std::array<ClassVector, kPositions>& call) const {
        double total = 0.0;
        const ClassVector values = jam_values(call);
        for (std::size_t i = 0; i < kClasses; ++i) {
            const double best = std::max(values[i], fold_value());
            const double played = jam[i] * values[i] + (1.0 - jam[i]) * fold_value();
            total += in_.prior[i] * (best - played);
        }
        for (const int c : callers_) {
            const ClassVector gains = call_gains(c, jam);
            const ClassVector& calls = call[static_cast<std::size_t>(c)];
            double faced = 0.0;
            double gain = 0.0;
            for (std::size_t j = 0; j < kClasses; ++j) {
                double jammed = 0.0;
                for (std::size_t i = 0; i < kClasses; ++i) jammed += in_.given[j][i] * jam[i];
                const double weight = in_.prior[j] * jammed;
                faced += weight;
                gain += weight * (std::max(gains[j], 0.0) - calls[j] * gains[j]);
            }
            if (faced > 0.0) total += gain / faced;
        }
        return total;
    }

    Result solve(int iterations) const {
        Result r;
        r.jam.fill(1.0);
        for (const int c : callers_) r.call[static_cast<std::size_t>(c)].fill(0.5);

        std::array<std::array<bool, kClasses>, kPositions + 1> published{};
        const auto publish = [&](int t) {
            bool changed = false;
            const auto check = [&](std::size_t slot, const ClassVector& strategy) {
                for (std::size_t k = 0; k < kClasses; ++k) {
                    const bool in_set = strategy[k] >= 0.5;
                    if (published[slot][k] != in_set) changed = true;
                    published[slot][k] = in_set;
                }
            };
            check(kPositions, r.jam);
            for (const int c : callers_) check(static_cast<std::size_t>(c), r.call[static_cast<std::size_t>(c)]);
            if (changed) r.last_change = t;
        };
        publish(0);

        for (int t = 1; t <= iterations; ++t) {
            const double step = 1.0 / (t + 1.0);
            const ClassVector values = jam_values(r.call);
            for (std::size_t i = 0; i < kClasses; ++i) {
                const double best = values[i] > fold_value() ? 1.0 : 0.0;
                r.jam[i] += (best - r.jam[i]) * step;
            }
            for (const int c : callers_) {
                const ClassVector gains = call_gains(c, r.jam);
                ClassVector& calls = r.call[static_cast<std::size_t>(c)];
                for (std::size_t j = 0; j < kClasses; ++j) calls[j] += ((gains[j] > 0.0 ? 1.0 : 0.0) - calls[j]) * step;
            }
            publish(t);
        }

        r.nash_conv_mixed = nash_conv(r.jam, r.call);
        Result pure = r;
        for (double& x : pure.jam) x = x >= 0.5 ? 1.0 : 0.0;
        for (ClassVector& calls : pure.call) {
            for (double& x : calls) x = x >= 0.5 ? 1.0 : 0.0;
        }
        r.nash_conv_pure = nash_conv(pure.jam, pure.call);
        return r;
    }

private:
    // The pot when caller c calls: both stacks, the blinds of everybody else and
    // the antes.
    double pot(int caller) const { return 2.0 * stack_ + 1.5 + dead_ - blind_of(jammer_) - blind_of(caller); }

    const Inputs& in_;
    int jammer_;
    double stack_;
    double dead_;
    std::vector<int> callers_;
};

std::array<std::uint64_t, detail::kHandSetWords> bits_of(const ClassVector& strategy) {
    std::array<std::uint64_t, detail::kHandSetWords> words{};
    for (std::size_t k = 0; k < kClasses; ++k) {
        if (strategy[k] >= 0.5) words[k / 64] |= std::uint64_t{1} << (k % 64);
    }
    return words;
}

double percent_of(const ClassVector& strategy, const Inputs& in) {
    int combos = 0;
    for (std::size_t k = 0; k < kClasses; ++k) {
        if (strategy[k] >= 0.5) combos += in.combos[k];
    }
    return 100.0 * combos / kTotalCombos;
}

bool parse_int(std::string_view text, int& value) {
    try {
        std::size_t used = 0;
        value = std::stoi(std::string(text), &used);
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const std::string_view value = argv[++i];
        if (arg == "--iterations") {
            if (!parse_int(value, options.iterations) || options.iterations <= 0) return false;
        } else if (arg == "--threads") {
            if (!parse_int(value, options.threads) || options.threads < 0) return false;
        } else if (arg == "--out") {
            options.out = std::string(value);
        } else if (arg == "--ante-out") {
            options.ante_out = std::string(value);
        } else {
            return false;
        }
    }
    return true;
}

void write_words(std::ostream& out, const std::array<std::uint64_t, detail::kHandSetWords>& words) {
    out << '{';
    for (std::size_t w = 0; w < words.size(); ++w) {
        out << (w == 0 ? "" : ", ") << "0x" << std::hex << std::setw(16) << std::setfill('0') << words[w]
            << std::dec << "ULL";
    }
    out << '}';
}

std::string_view name_of(int position) { return to_string(static_cast<Position>(position)); }

// The jam and call arrays' bodies for one dead-money level, at `indent`.
template <typename ResultOf>
void write_tables(std::ostream& out, const std::string& indent, const ResultOf& result_of, bool jam) {
    if (jam) {
        for (int jammer = 0; jammer < kJammers; ++jammer) {
            out << indent << "{  // " << name_of(jammer) << '\n';
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                out << indent << "    ";
                write_words(out, bits_of(result_of(jammer, stack).jam));
                out << ",  // " << stack << " BB\n";
            }
            out << indent << "},\n";
        }
        return;
    }
    for (int jammer = 0; jammer < kJammers; ++jammer) {
        out << indent << "{  // " << name_of(jammer) << " jams\n";
        for (int caller = 0; caller < kPositions; ++caller) {
            if (caller <= jammer) {
                out << indent << "    {},  // " << name_of(caller) << ": acts before the jammer\n";
                continue;
            }
            out << indent << "    {  // " << name_of(caller) << " calls\n";
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                out << indent << "        ";
                write_words(out, bits_of(result_of(jammer, stack).call[static_cast<std::size_t>(caller)]));
                out << ",  // " << stack << " BB\n";
            }
            out << indent << "    },\n";
        }
        out << indent << "},\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "usage: gen_push_fold [--iterations N] [--threads N] [--out PATH] [--ante-out PATH] [--verbose]\n";
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    const std::unique_ptr<const Inputs> owned_inputs = build_inputs();
    const Inputs& inputs = *owned_inputs;

    // Level 0 is no ante; level a > 0 is detail::kPushFoldAnteDeadBb[a - 1].
    constexpr int kLevels = detail::kPushFoldAnteLevels + 1;
    const auto dead_of = [](int level) { return level == 0 ? 0.0 : detail::kPushFoldAnteDeadBb[level - 1]; };
    struct Job {
        int level = 0;
        int jammer = 0;
        int stack = 0;
    };
    std::vector<Job> jobs;
    for (int level = 0; level < kLevels; ++level) {
        for (int jammer = 0; jammer < kJammers; ++jammer) {
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                jobs.push_back({level, jammer, stack});
            }
        }
    }
    std::vector<Result> results(jobs.size());
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t k = next.fetch_add(1); k < jobs.size(); k = next.fetch_add(1)) {
            results[k] = Game(inputs, jobs[k].jammer, jobs[k].stack, dead_of(jobs[k].level)).solve(options.iterations);
        }
    };
    int threads = options.threads;
    if (threads == 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    {
        std::vector<std::jthread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (int t = 1; t < threads; ++t) workers.emplace_back(work);
        work();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto result_at = [&](int level, int jammer, int stack) -> const Result& {
        return results[static_cast<std::size_t>((level * kJammers + jammer) * kStacks + stack - kPushFoldMinStackBb)];
    };
    const auto result_of = [&](int jammer, int stack) -> const Result& { return result_at(0, jammer, stack); };

    std::ofstream out(options.out);
    if (!out) {
        std::cerr << "cannot write " << options.out << '\n';
        return 1;
    }
    out << "// Generated by tools/gen_push_fold.cpp --iterations " << options.iterations
        << " (fictitious play, blinds 0.5/1 BB, no ante). Do not edit.\n"
        << "// Push/fold sets as described in strategy/push_fold.h: bit k is the class with\n"
        << "// HandClass::index() k. Multiway jams assume one caller; see the generator.\n"
        << "#include \"strategy/push_fold.h\"\n\n"
        << "namespace poker::detail {\n\n"
        << "const int kPushFoldIterations = " << options.iterations << ";\n\n"
        << "const std::uint64_t kPushFoldJam[kPushFoldJammerCount][kPushFoldStackCount][kHandSetWords] = {\n";
    write_tables(out, "    ", result_of, true);
    out << "};\n\n"
        << "const std::uint64_t kPushFoldCall[kPushFoldJammerCount][kPushFoldPositionCount][kPushFoldStackCount]\n"
        << "                                 [kHandSetWords] = {\n";
    write_tables(out, "    ", result_of, false);
    out << "};\n\n}  // namespace poker::detail\n";
    if (!out) {
        std::cerr << "write failed: " << options.out << '\n';
        return 1;
    }

    std::ofstream ante(options.ante_out);
    if (!ante) {
        std::cerr << "cannot write " << options.ante_out << '\n';
        return 1;
    }
    ante << "// Generated by tools/gen_push_fold.cpp --iterations " << options.iterations
         << " (fictitious play, blinds 0.5/1 BB, antes as dead money). Do not edit.\n"
         << "// As push_fold_data.cpp, for each dead-money level in kPushFoldAnteDeadBb.\n"
         << "#include \"strategy/push_fold.h\"\n\n"
         << "namespace poker::detail {\n\n"
         << "const std::uint64_t kPushFoldAnteJam[kPushFoldAnteLevels][kPushFoldJammerCount][kPushFoldStackCount]\n"
         << "                                    [kHandSetWords] = {\n";
    for (int level = 1; level < kLevels; ++level) {
        ante << "    {  // " << dead_of(level) << " BB dead\n";
        write_tables(ante, "        ", [&](int jammer, int stack) -> const Result& { return result_at(level, jammer, stack); },
                     true);
        ante << "    },\n";
    }
    ante << "};\n\n"
         << "const std::uint64_t kPushFoldAnteCall[kPushFoldAnteLevels][kPushFoldJammerCount][kPushFoldPositionCount]\n"
         << "                                     [kPushFoldStackCount][kHandSetWords] = {\n";
    for (int level = 1; level < kLevels; ++level) {
        ante << "    {  // " << dead_of(level) << " BB dead\n";
        write_tables(ante, "        ", [&](int jammer, int stack) -> const Result& { return result_at(level, jammer, stack); },
                     false);
        ante << "    },\n";
    }
    ante << "};\n\n}  // namespace poker::detail\n";
    if (!ante) {
        std::cerr << "write failed: " << options.ante_out << '\n';
        return 1;
    }

    // Summary: jam and call shares, and convergence.
    int last_change = 0;
    double worst_mixed = 0.0;
    double worst_pure = 0.0;
    for (const Result& r : results) {
        last_change = std::max(last_change, r.last_change);
        worst_mixed = std::max(worst_mixed, r.nash_conv_mixed);
        worst_pure = std::max(worst_pure, r.nash_conv_pure);
    }
    for (int level = 0; level < kLevels; ++level) {
        std::cerr << std::fixed << std::setprecision(1) << "jam % of combos by stack (BB), " << dead_of(level)
                  << " BB dead:\n     ";
        for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) std::cerr << std::setw(6) << stack;
        std::cerr << '\n';
        for (int jammer = 0; jammer < kJammers; ++jammer) {
            std::cerr << std::setw(5) << name_of(jammer);
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                std::cerr << std::setw(6) << percent_of(result_at(level, jammer, stack).jam, inputs);
            }
            std::cerr << '\n';
        }
    }
    std::cerr << "call % of combos against a lone jam, by stack, no ante:\n";
    for (int jammer = 0; jammer < kJammers; ++jammer) {
        for (int caller = jammer + 1; caller < kPositions; ++caller) {
            std::cerr << std::setw(4) << name_of(caller) << " v " << std::setw(3) << name_of(jammer);
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                std::cerr << std::setw(6)
                          << percent_of(result_of(jammer, stack).call[static_cast<std::size_t>(caller)], inputs);
            }
            std::cerr << '\n';
        }
    }
    if (options.verbose) {
        std::cerr << std::setprecision(3) << "per game: last set change, NashConv mBB (average, published)\n";
        for (std::size_t k = 0; k < jobs.size(); ++k) {
            std::cerr << std::setw(4) << dead_of(jobs[k].level) << std::setw(5) << name_of(jobs[k].jammer)
                      << std::setw(4) << jobs[k].stack << " BB"
                      << std::setw(6) << results[k].last_change << std::setw(9) << 1000.0 * results[k].nash_conv_mixed
                      << std::setw(9) << 1000.0 * results[k].nash_conv_pure << '\n';
        }
    }
    std::cerr << std::setprecision(3) << "convergence: last published-set change at iteration " << last_change
              << " of " << options.iterations << "; worst NashConv " << 1000.0 * worst_mixed
              << " mBB (average strategies), " << 1000.0 * worst_pure << " mBB (published sets)\n"
              << "wrote " << options.out << " and " << options.ante_out << ": " << jobs.size() << " games in " << std::setprecision(2) << seconds
              << " s on " << threads << " threads\n";
    return 0;
}
