// Headless test harness: plays freezeouts between random legal-action players,
// checks engine invariants at every step, and exits non-zero on any violation.
//
//   poker_cli [--games N] [--seed S] [--verbose]

#include <array>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

#include "engine/game_engine.h"

using namespace poker;

namespace {

constexpr int kMaxHandsPerGame = 3000;  // Fixed blinds and passive play can run long

struct Stats {
    long long games = 0;
    long long games_to_winner = 0;
    long long hands = 0;
    long long actions = 0;
    long long showdowns = 0;
    long long multi_pot_hands = 0;
    long long refunds = 0;
    long long split_pots = 0;
    long long heads_up_hands = 0;
    long long short_all_ins = 0;
    long long illegal_rejected = 0;
    long long violations = 0;
};

Stats stats;
int current_game = 0;

void fail(const GameState& state, const std::string& message) {
    ++stats.violations;
    std::cerr << "VIOLATION game " << current_game << " hand " << state.hand_number << ": " << message << '\n';
}

int total_chips(const GameState& state) {
    int total = 0;
    for (const Seat& seat : state.seats) total += seat.stack + seat.total_bet;
    return total;
}

int table_chips_between_hands(const GameState& state) {
    int total = 0;
    for (const Seat& seat : state.seats) total += seat.stack;
    return total;
}

void check_during_hand(const GameState& state, int expected_chips) {
    if (total_chips(state) != expected_chips) fail(state, "chips not conserved during hand");

    std::bitset<kDeckSize> seen;
    const auto see = [&](Card card) {
        if (seen.test(card.id)) fail(state, "duplicate card " + card.to_string());
        seen.set(card.id);
    };
    for (const Card card : state.board) see(card);
    for (const Seat& seat : state.seats) {
        if (seat.stack < 0 || seat.street_bet < 0) fail(state, "negative chips");
        if (seat.status == SeatStatus::Busted && seat.hole) fail(state, "busted seat was dealt in");
        if (seat.status != SeatStatus::Busted && !seat.hole) fail(state, "live seat has no cards");
        if (seat.hole) {
            see((*seat.hole)[0]);
            see((*seat.hole)[1]);
        }
    }
    if (state.seats[state.button].status == SeatStatus::Busted) fail(state, "button on busted seat");
}

void check_after_hand(const GameState& state, int expected_chips) {
    if (table_chips_between_hands(state) != expected_chips) fail(state, "chips not conserved after hand");
    if (!state.last_result) {
        fail(state, "finished hand has no result");
        return;
    }

    const HandResult& result = *state.last_result;
    int pots_total = 0;
    for (const Pot& pot : result.pots) {
        pots_total += pot.amount;
        if (pot.eligible.empty()) fail(state, "pot with no eligible player");
    }
    int payouts_total = 0;
    for (const Payout& payout : result.payouts) payouts_total += payout.amount;
    const int refund = result.refund ? result.refund->amount : 0;

    if (pots_total != payouts_total) fail(state, "payouts do not match pots");
    if (pots_total + refund != state.total_pot()) fail(state, "pots and refund do not match chips bet");
    if (result.showdown && state.board.size() != 5) fail(state, "showdown without a full board");

    for (const Seat& seat : state.seats) {
        if ((seat.stack == 0) != (seat.status == SeatStatus::Busted)) fail(state, "bust status out of step with stack");
    }

    ++stats.hands;
    if (result.showdown) ++stats.showdowns;
    if (result.pots.size() > 1) ++stats.multi_pot_hands;
    if (result.refund) ++stats.refunds;
    if (result.showdown) {
        for (const Pot& pot : result.pots) {
            HandRank best = kWorstHandRank + 1;
            int winners = 0;
            for (const int seat : pot.eligible) {
                const HandRank rank = *result.showdown_ranks[seat];
                if (rank < best) {
                    best = rank;
                    winners = 0;
                }
                if (rank == best) ++winners;
            }
            if (winners > 1) ++stats.split_pots;
        }
    }
}

// Every clearly illegal action must be refused and leave the state untouched.
void probe_illegal_actions(GameEngine& engine, const LegalActions& legal) {
    const auto refuse = [&](Action action, const char* what) {
        const int seat = *engine.state().to_act;
        const int stack = engine.state().seats[seat].stack;
        const std::size_t action_count = engine.state().actions.size();
        if (engine.apply_action(action)) {
            fail(engine.state(), std::string("accepted illegal action: ") + what);
            return;
        }
        ++stats.illegal_rejected;
        if (engine.state().seats[seat].stack != stack || engine.state().actions.size() != action_count) {
            fail(engine.state(), std::string("rejected action changed state: ") + what);
        }
    };

    if (!legal.allows(ActionType::Check)) refuse({ActionType::Check}, "check facing a bet");
    if (!legal.allows(ActionType::Call)) refuse({ActionType::Call}, "call with nothing to call");

    for (const ActionType type : {ActionType::Bet, ActionType::Raise}) {
        if (legal.allows(type)) {
            if (legal.min_raise_to > 0) refuse({type, legal.min_raise_to - 1}, "raise below minimum");
            refuse({type, legal.max_raise_to + 1}, "raise above stack");
        } else {
            refuse({type, 1'000'000}, "bet or raise when not allowed");
        }
    }
}

Action random_action(const LegalActions& legal, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double roll = unit(rng);

    if (legal.allows(ActionType::AllIn) && roll < 0.06) return {ActionType::AllIn};
    for (const ActionType type : {ActionType::Bet, ActionType::Raise}) {
        if (legal.allows(type) && roll < 0.30) {
            // Skew towards small sizes, so short all-ins and re-raises both occur.
            const double fraction = unit(rng) * unit(rng);
            const int range = legal.max_raise_to - legal.min_raise_to;
            return {type, legal.min_raise_to + static_cast<int>(range * fraction)};
        }
    }
    if (legal.allows(ActionType::Check)) return {ActionType::Check};
    if (legal.allows(ActionType::Call) && roll < 0.75) return {ActionType::Call};
    return {ActionType::Fold};
}

void print_hand_header(const GameState& state) {
    std::cout << "\n=== Game " << current_game << " Hand " << state.hand_number << " ===\n";
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state.seats[seat];
        if (s.status == SeatStatus::Busted && !s.hole) {
            std::cout << "  Seat " << seat << ": BUSTED\n";
            continue;
        }
        std::cout << "  Seat " << seat << " " << to_string(*state.position_of(seat)) << "  "
                  << (*s.hole)[0].to_string() << (*s.hole)[1].to_string() << "  behind " << s.stack
                  << "  bet " << s.total_bet << '\n';
    }
}

void print_new_actions(const GameState& state, std::size_t& printed) {
    for (; printed < state.actions.size(); ++printed) {
        const ActionRecord& a = state.actions[printed];
        std::cout << "  [" << to_string(a.street) << "] Seat " << a.seat << " " << to_string(a.type);
        if (a.chips_added > 0) std::cout << " +" << a.chips_added << " (to " << a.street_total << ")";
        if (a.all_in) std::cout << " ALL-IN";
        std::cout << '\n';
    }
}

void print_hand_result(const GameState& state) {
    std::cout << "  Board:";
    for (const Card card : state.board) std::cout << ' ' << card.to_string();
    std::cout << '\n';

    const HandResult& result = *state.last_result;
    for (std::size_t i = 0; i < result.pots.size(); ++i) {
        std::cout << "  " << (i == 0 ? "Main pot" : "Side pot") << " " << result.pots[i].amount << " eligible:";
        for (const int seat : result.pots[i].eligible) std::cout << ' ' << seat;
        std::cout << '\n';
    }
    if (result.refund) std::cout << "  Refund " << result.refund->amount << " to seat " << result.refund->seat << '\n';
    for (const Payout& payout : result.payouts) {
        std::cout << "  Seat " << payout.seat << " wins " << payout.amount;
        if (result.showdown_ranks[payout.seat]) {
            std::cout << " with " << describe_hand(*result.showdown_ranks[payout.seat]);
        }
        std::cout << '\n';
    }
    std::cout << "  Stacks:";
    for (const Seat& seat : state.seats) std::cout << ' ' << seat.stack;
    std::cout << '\n';
}

// Varied stack depths per game so short-stack and side-pot paths get exercised.
GameConfig random_config(std::mt19937_64& rng) {
    GameConfig config;
    config.seed = rng();
    config.hero_seat = (rng() % 2 == 0) ? std::optional<int>(static_cast<int>(rng() % kMaxSeats)) : std::nullopt;

    if (rng() % 3 != 0) {
        std::array<int, kMaxSeats> stacks{};
        for (int& stack : stacks) stack = 1 + static_cast<int>(rng() % 2500);
        const int empty_seats = static_cast<int>(rng() % 5);  // 2 to 6 players at the start
        for (int i = 0; i < empty_seats; ++i) stacks[rng() % kMaxSeats] = 0;

        int live = 0;
        for (const int stack : stacks) live += stack > 0 ? 1 : 0;
        if (live < 2) {
            stacks[0] = 500;
            stacks[1] = 500;
        }
        if (config.hero_seat && stacks[*config.hero_seat] == 0) stacks[*config.hero_seat] = 300;
        config.stacks = stacks;
    }
    return config;
}

void play_game(std::mt19937_64& rng, bool verbose) {
    GameEngine engine(random_config(rng));
    const int chips = table_chips_between_hands(engine.state());
    ++stats.games;

    while (engine.state().phase == GamePhase::BetweenHands && engine.state().hand_number < kMaxHandsPerGame) {
        engine.start_hand();
        const GameState& state = engine.state();

        int players = 0;
        for (const Seat& seat : state.seats) players += seat.hole ? 1 : 0;
        if (players == 2) ++stats.heads_up_hands;

        if (verbose) print_hand_header(state);
        std::size_t printed = 0;

        while (state.phase == GamePhase::Betting) {
            check_during_hand(state, chips);
            const LivePots live = engine.live_pots();
            int live_total = live.uncalled;
            for (const Pot& pot : live.pots) {
                live_total += pot.amount;
                if (pot.eligible.empty()) fail(state, "live pot with nobody eligible");
            }
            if (live_total != state.total_pot()) fail(state, "live pots do not match chips bet");
            const LegalActions legal = engine.legal_actions();
            if (legal.mask == 0) {
                fail(state, "betting phase with no legal action");
                return;
            }
            probe_illegal_actions(engine, legal);

            const int level_before = state.current_bet;
            const int raise_size_before = state.last_raise_size;
            const Action action = random_action(legal, rng);
            if (!engine.apply_action(action)) {
                fail(state, "legal action rejected");
                return;
            }
            ++stats.actions;

            const ActionRecord& last = state.actions.back();
            if (last.all_in && last.street_total > level_before && last.street_total - level_before < raise_size_before) {
                ++stats.short_all_ins;
            }
            if (verbose) print_new_actions(state, printed);
        }

        check_after_hand(state, chips);
        if (verbose) print_hand_result(state);
    }

    const SessionSummary summary = engine.session_summary();
    if (summary.winner) ++stats.games_to_winner;
    if (engine.state().phase == GamePhase::SessionOver && !summary.winner && !summary.hero_busted) {
        fail(engine.state(), "session over with no winner and hero alive");
    }
}

}  // namespace

int main(int argc, char** argv) {
    int games = 200;
    std::uint64_t seed = 1;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--games" && i + 1 < argc) {
            games = std::atoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::cerr << "Usage: poker_cli [--games N] [--seed S] [--verbose]\n";
            return 2;
        }
    }

    std::mt19937_64 rng(seed);
    for (current_game = 1; current_game <= games; ++current_game) play_game(rng, verbose);

    std::cout << "\nGames:               " << stats.games
              << "\n  played to a winner " << stats.games_to_winner
              << "\nHands:               " << stats.hands
              << "\n  heads-up           " << stats.heads_up_hands
              << "\n  showdowns          " << stats.showdowns
              << "\n  with side pots     " << stats.multi_pot_hands
              << "\n  uncalled refunds   " << stats.refunds
              << "\nSplit pots:          " << stats.split_pots
              << "\nActions:             " << stats.actions
              << "\nShort all-in raises: " << stats.short_all_ins
              << "\nIllegal rejected:    " << stats.illegal_rejected
              << "\nInvariant violations: " << stats.violations << '\n';

    return stats.violations == 0 ? 0 : 1;
}
