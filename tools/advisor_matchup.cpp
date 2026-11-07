// Plays the Training Advisor against tables of one bot type and compares its
// Baseline advice with one player-type line (Stage 9 validation).
//
//   advisor_matchup [--hands N] [--matchup nit|station|maniac|gto|all]
//                   [--advisor-rollouts N] [--range-rollouts N] [--bot-rollouts N]
//                   [--threads N] [--seed S] [--csv PATH]
//
// Seat 0 is an AdvisorBot: it calls TrainingAdvisor::analyze on its PlayerView and
// plays either the Baseline action (the hint's own) or one archetype line's action
// (hint.versus), falling back to Baseline if that line is missing. The other five
// seats all hold one bot type:
//   nit      Nit line vs 5 NitBots
//   station  Station line vs 5 LooseBots (the closest bot to a calling station)
//   maniac   Maniac line vs 5 tool-only maniacs: loose_profile with bluff_freq 0.6,
//            value_bet_strength 0.15 and bluff_raise_share 0.6
//   gto      every line vs 5 GtoBots: information only, the cost of applying an
//            exploit to the wrong opponents
//
// Paired design: each hand is played once per line from the same state. Every
// hand gets a fresh engine (deck seed), fresh bots (bot seeds) and a fresh advisor
// (advisor seed), all derived from (--seed, matchup, hand index), with 100 BB
// stacks and the button on hand index % 6. Only Hero's advice differs between the
// lines, so a hand's difference in Hero's result is the effect of the line (and of
// how the bots' own random streams fall after the play diverges). Reported per
// line: bb/100 with its standard error, and the paired difference
// (line - Baseline) in bb/100 with the standard error of the per-hand differences.
//
// Bots play their normal profiles but with --bot-rollouts Monte Carlo rollouts
// (default 500, the game uses 5000), so their equity estimates are noisier; that
// keeps the run time reasonable and is the same for every line.
//
// Each hand is checked: every action must be legal and the six stacks must still
// hold 6 x 100 BB afterwards. Exit status: 0 on success, 1 when a check fails, 2 on
// a usage or file error.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/training_advisor.h"
#include "bots/bots.h"
#include "engine/game_engine.h"
#include "range_text.h"

using namespace poker;

namespace {

constexpr std::string_view kUsage =
    "Usage: advisor_matchup [--hands N] [--matchup nit|station|maniac|gto|all]\n"
    "                       [--advisor-rollouts N] [--range-rollouts N] [--bot-rollouts N]\n"
    "                       [--threads N] [--seed S] [--csv PATH]\n"
    "  --hands             hands per matchup, each played once per line (default 3000)\n"
    "  --matchup           which table to play (default all)\n"
    "  --advisor-rollouts  advisor random-hand rollouts (default 1000)\n"
    "  --range-rollouts    advisor multiway range rollouts (default 1000)\n"
    "  --bot-rollouts      bot Monte Carlo rollouts (default 500; the game uses 5000)\n"
    "  --threads           advisor threads (default 1, for several processes at once)\n"
    "  --seed              base seed (default 1)\n"
    "  --csv               write one row per hand and line\n";

enum class Opponent : std::uint8_t { Nit, Loose, Maniac, Gto };

struct Matchup {
    std::string_view name;
    Opponent opponent;
    std::vector<Archetype> lines;  // Played besides Baseline
    bool informational;
};

const std::array<Matchup, 4>& matchups() {
    static const std::array<Matchup, 4> all{{
        {"nit", Opponent::Nit, {Archetype::Nit}, false},
        {"station", Opponent::Loose, {Archetype::Station}, false},
        {"maniac", Opponent::Maniac, {Archetype::Maniac}, false},
        {"gto", Opponent::Gto, {Archetype::Nit, Archetype::Station, Archetype::Maniac}, true},
    }};
    return all;
}

struct Options {
    int hands = 3000;
    std::string matchup = "all";
    int advisor_rollouts = 1000;
    int range_rollouts = 1000;
    int bot_rollouts = 500;
    int threads = 1;
    std::uint64_t seed = 1;
    std::optional<std::string> csv;
};

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << "advisor_matchup: " << message << "\n\n" << kUsage;
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
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            std::exit(0);
        }
        if (i + 1 >= argc) usage_error("unknown flag or missing value: " + std::string(arg));
        const std::string_view value = argv[++i];
        if (arg == "--hands") {
            options.hands = int_flag(arg, value, 1);
        } else if (arg == "--matchup") {
            options.matchup = std::string(value);
            bool known = options.matchup == "all";
            for (const Matchup& m : matchups()) known = known || m.name == value;
            if (!known) usage_error("--matchup needs nit, station, maniac, gto or all");
        } else if (arg == "--advisor-rollouts") {
            options.advisor_rollouts = int_flag(arg, value, 1);
        } else if (arg == "--range-rollouts") {
            options.range_rollouts = int_flag(arg, value, 1);
        } else if (arg == "--bot-rollouts") {
            options.bot_rollouts = int_flag(arg, value, 1);
        } else if (arg == "--threads") {
            options.threads = int_flag(arg, value, 1);
        } else if (arg == "--seed") {
            options.seed = static_cast<std::uint64_t>(int_flag(arg, value, 0));
        } else if (arg == "--csv") {
            options.csv = std::string(value);
        } else {
            usage_error("unknown flag: " + std::string(arg));
        }
    }
    return options;
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The tool's maniac: the Loose bot, bluffing and value-betting far more often.
PersonalityProfile maniac_profile(int rollouts) {
    PersonalityProfile p = loose_profile(rollouts);
    p.name = "Maniac";
    p.bluff_freq = 0.6;
    p.value_bet_strength = 0.15;
    p.bluff_raise_share = 0.6;
    return p;
}

std::unique_ptr<IBotStrategy> make_opponent(Opponent opponent, std::uint64_t seed, int rollouts) {
    switch (opponent) {
        case Opponent::Nit:
            return make_bot(BotKind::Nit, seed, rollouts);
        case Opponent::Loose:
            return make_bot(BotKind::Loose, seed, rollouts);
        case Opponent::Maniac:
            return std::make_unique<ProfileBot>(maniac_profile(rollouts), seed);
        case Opponent::Gto:
            break;
    }
    return make_bot(BotKind::Gto, seed, rollouts);
}

// Seat 0's player: the advisor's Baseline action, or one archetype line's.
class AdvisorBot {
public:
    AdvisorBot(std::uint64_t seed, AdvisorOptions options, std::optional<Archetype> line)
        : advisor_(seed, with_lines(options, line.has_value())), line_(line) {}

    Action decide(const PlayerView& view) {
        const HintRecommendation hint = advisor_.analyze(view);
        ++decisions_;
        if (!line_) return hint.action;
        const std::size_t index = static_cast<std::size_t>(*line_) - 1;  // versus: Nit, Station, Maniac
        const std::optional<AdviceLine>& advice = hint.versus[index];
        if (!advice) {
            ++fallbacks_;
            return hint.action;
        }
        if (advice->action.type != hint.action.type || advice->action.amount != hint.action.amount) ++differs_;
        return advice->action;
    }

    int decisions() const { return decisions_; }
    int differs() const { return differs_; }
    int fallbacks() const { return fallbacks_; }

private:
    // Baseline's numbers do not depend on the lines, so Baseline skips them.
    static AdvisorOptions with_lines(AdvisorOptions options, bool lines) {
        options.archetype_lines = lines;
        return options;
    }

    TrainingAdvisor advisor_;
    std::optional<Archetype> line_;
    int decisions_ = 0;
    int differs_ = 0;
    int fallbacks_ = 0;
};

struct HandResult {
    int delta = 0;  // Hero's chips after the hand minus 100 BB
    int decisions = 0;
    int differs = 0;
    int fallbacks = 0;
};

constexpr int kHero = 0;

// One hand from fresh 100 BB stacks. Returns nothing when a check fails.
std::optional<HandResult> play_hand(const Matchup& matchup, std::optional<Archetype> line, int hand_index,
                                    std::uint64_t hand_seed, const Options& options) {
    GameConfig config;
    config.seed = splitmix64(hand_seed ^ 0x01);
    config.hero_seat = kHero;
    config.first_button = hand_index % kMaxSeats;
    GameEngine engine(config);

    AdvisorOptions advisor_options;
    advisor_options.rollouts = options.advisor_rollouts;
    advisor_options.range_rollouts = options.range_rollouts;
    advisor_options.threads = options.threads;
    AdvisorBot hero(splitmix64(hand_seed ^ 0x09), advisor_options, line);

    std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots;
    for (int s = 1; s < kMaxSeats; ++s) {
        bots[static_cast<std::size_t>(s)] = make_opponent(
            matchup.opponent, splitmix64(hand_seed ^ (0x10 + static_cast<std::uint64_t>(s))), options.bot_rollouts);
    }

    engine.start_hand();
    for (int s = 1; s < kMaxSeats; ++s) bots[static_cast<std::size_t>(s)]->begin_hand(engine.state().hand_number);
    while (engine.state().to_act) {
        const int s = *engine.state().to_act;
        const PlayerView view = engine.player_view(s);
        const Action action = s == kHero ? hero.decide(view) : bots[static_cast<std::size_t>(s)]->decide(view);
        if (!engine.apply_action(action)) {
            std::cerr << "advisor_matchup: illegal action by position " << s << " in hand " << hand_index << "\n";
            return std::nullopt;
        }
    }
    if (engine.state().phase == GamePhase::Betting) {
        std::cerr << "advisor_matchup: hand " << hand_index << " stopped with nobody to act\n";
        return std::nullopt;
    }

    int total = 0;
    for (const auto& s : engine.state().seats) total += s.stack;
    if (total != kMaxSeats * config.starting_stack) {
        std::cerr << "advisor_matchup: chips not conserved in hand " << hand_index << ": " << total << "\n";
        return std::nullopt;
    }
    HandResult result;
    result.delta = engine.state().seats[static_cast<std::size_t>(kHero)].stack - config.starting_stack;
    result.decisions = hero.decisions();
    result.differs = hero.differs();
    result.fallbacks = hero.fallbacks();
    return result;
}

// Mean and standard error of a sample, in bb/100.
struct Summary {
    double mean = 0.0;
    double se = 0.0;
};

Summary summarize(const std::vector<double>& chips, int big_blind) {
    const double n = static_cast<double>(chips.size());
    double sum = 0.0;
    for (const double x : chips) sum += x;
    const double mean = sum / n;
    double squares = 0.0;
    for (const double x : chips) squares += (x - mean) * (x - mean);
    const double sd = chips.size() > 1 ? std::sqrt(squares / (n - 1.0)) : 0.0;
    const double scale = 100.0 / big_blind;
    return {mean * scale, sd / std::sqrt(n) * scale};
}

struct LineTotals {
    std::vector<double> deltas;
    long decisions = 0;
    long differs = 0;
    long fallbacks = 0;
};

int run_matchup(std::size_t matchup_index, const Options& options, std::ofstream* csv) {
    const Matchup& matchup = matchups()[matchup_index];
    const int big_blind = GameConfig{}.big_blind;
    std::vector<std::optional<Archetype>> lines{std::nullopt};
    for (const Archetype a : matchup.lines) lines.emplace_back(a);
    std::vector<LineTotals> totals(lines.size());

    const auto start = std::chrono::steady_clock::now();
    for (int h = 0; h < options.hands; ++h) {
        const std::uint64_t hand_seed =
            splitmix64(splitmix64(options.seed ^ (0x1000 * (matchup_index + 1))) + static_cast<std::uint64_t>(h));
        for (std::size_t k = 0; k < lines.size(); ++k) {
            const std::optional<HandResult> result = play_hand(matchup, lines[k], h, hand_seed, options);
            if (!result) return 1;
            LineTotals& t = totals[k];
            t.deltas.push_back(result->delta);
            t.decisions += result->decisions;
            t.differs += result->differs;
            t.fallbacks += result->fallbacks;
            if (csv) {
                *csv << matchup.name << ',' << h << ',' << h % kMaxSeats << ','
                     << (lines[k] ? to_string(*lines[k]) : std::string_view("Baseline")) << ',' << result->delta
                     << ',' << result->decisions << ',' << result->differs << ',' << result->fallbacks << '\n';
            }
        }
        if ((h + 1) % 250 == 0) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cerr << matchup.name << ": " << h + 1 << " hands, " << static_cast<int>(seconds) << " s\n";
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("\n### %.*s: Hero vs 5 x %s%s\n\n", static_cast<int>(matchup.name.size()), matchup.name.data(),
                matchup.opponent == Opponent::Nit     ? "NitBot"
                : matchup.opponent == Opponent::Loose ? "LooseBot"
                : matchup.opponent == Opponent::Maniac ? "maniac (tool profile)"
                                                       : "GtoBot",
                matchup.informational ? " (information only)" : "");
    std::printf("| line | hands | bb/100 | SE | diff vs Baseline bb/100 | SE of diff | diff / SE | decisions "
                "| differs from Baseline | fallbacks |\n|---|---|---|---|---|---|---|---|---|---|\n");
    const Summary base = summarize(totals[0].deltas, big_blind);
    std::printf("| Baseline | %d | %.1f | %.1f | | | | %ld | | |\n", options.hands, base.mean, base.se,
                totals[0].decisions);
    for (std::size_t k = 1; k < lines.size(); ++k) {
        const Summary own = summarize(totals[k].deltas, big_blind);
        std::vector<double> diff(totals[k].deltas.size());
        for (std::size_t h = 0; h < diff.size(); ++h) diff[h] = totals[k].deltas[h] - totals[0].deltas[h];
        const Summary paired = summarize(diff, big_blind);
        const std::string_view name = to_string(*lines[k]);
        std::printf("| %.*s | %d | %.1f | %.1f | %+.1f | %.1f | %+.2f | %ld | %.1f%% | %ld |\n",
                    static_cast<int>(name.size()), name.data(), options.hands, own.mean, own.se, paired.mean,
                    paired.se, paired.se > 0.0 ? paired.mean / paired.se : 0.0, totals[k].decisions,
                    totals[k].decisions > 0 ? 100.0 * static_cast<double>(totals[k].differs) /
                                                  static_cast<double>(totals[k].decisions)
                                            : 0.0,
                    totals[k].fallbacks);
    }
    std::printf("\n%.0f s for %d hands x %zu lines\n", seconds, options.hands, lines.size());
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    std::ofstream csv_file;
    if (options.csv) {
        csv_file.open(*options.csv);
        if (!csv_file) {
            std::cerr << "advisor_matchup: cannot write " << *options.csv << "\n";
            return 2;
        }
        csv_file << "matchup,hand,button,line,hero_delta_chips,decisions,differs,fallbacks\n";
    }
    std::printf("advisor_matchup: %d hands, advisor rollouts %d, range rollouts %d, bot rollouts %d, seed %llu\n",
                options.hands, options.advisor_rollouts, options.range_rollouts, options.bot_rollouts,
                static_cast<unsigned long long>(options.seed));
    for (std::size_t m = 0; m < matchups().size(); ++m) {
        if (options.matchup != "all" && matchups()[m].name != options.matchup) continue;
        if (run_matchup(m, options, options.csv ? &csv_file : nullptr) != 0) return 1;
    }
    return 0;
}
