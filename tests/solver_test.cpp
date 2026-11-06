#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "core/hand_evaluator.h"
#include "range/combos.h"
#include "range/weighted_range.h"
#include "solver/betting_tree.h"
#include "solver/dcfr.h"
#include "solver/postflop_solver.h"
#include "solver/showdown.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

Card card(const char* text) { return Card::parse(text).value(); }

std::vector<Card> cards(std::initializer_list<const char*> texts) {
    std::vector<Card> result;
    for (const char* text : texts) result.push_back(card(text));
    return result;
}

std::array<Card, 5> five(const std::vector<Card>& board) { return {board[0], board[1], board[2], board[3], board[4]}; }

HandRank rank_of(int combo, const std::vector<Card>& board) {
    const std::array<Card, 2> hole = combo_cards(combo);
    return evaluate7(hole, five(board));
}

// Raw generator bits only: the standard distributions differ between libraries.
double unit(std::mt19937_64& rng) { return static_cast<double>(rng() >> 11) * 0x1.0p-53; }

// --- Kuhn poker: the DCFR primitives on a game with a known value -------------

// Cards 0 = J, 1 = Q, 2 = K; each player antes 1 and may bet 1. Histories:
// 0 = start (player 1 acts), 1 = player 1 checked (player 2 acts), 2 = player 1 bet
// (player 2 acts), 3 = check then bet (player 1 acts). Action 0 passes (check or
// fold), action 1 bets (bet or call). An information set is card * 4 + history.
class Kuhn {
public:
    void iterate(const DcfrParams& params, int iteration) {
        const DcfrDiscount discount = dcfr_discount(params, iteration);
        for (int traverser = 0; traverser < 2; ++traverser) {
            delta_regret_ = {};
            delta_sum_ = {};
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    if (a != b) walk({a, b}, 0, traverser, 1.0, 1.0 / 6.0);
                }
            }
            // Each information set is discounted once per iteration, after all six deals.
            for (std::size_t i = 0; i < regret_.size(); ++i) {
                if (actor(static_cast<int>(i % 4)) != traverser) continue;
                for (std::size_t a = 0; a < 2; ++a) {
                    regret_[i][a] = discounted_regret(regret_[i][a], discount) + delta_regret_[i][a];
                    sum_[i][a] = sum_[i][a] * discount.strategy + delta_sum_[i][a];
                }
            }
        }
    }

    // Probability of action 1 (bet or call) in the average strategy.
    double average_bet(int card, int history) const {
        std::array<double, 2> average{};
        const std::array<double, 2>& sum = sum_[static_cast<std::size_t>(card * 4 + history)];
        regret_match(std::span<const double>(sum), 2, 1, std::span<double>(average));
        return average[1];
    }

    // Player 1's expected value when both play their average strategies.
    double value() const {
        double total = 0.0;
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                if (a != b) total += expected({a, b}, 0) / 6.0;
            }
        }
        return total;
    }

private:
    static int actor(int history) { return history == 0 || history == 3 ? 0 : 1; }

    // Player 1's payoff when `action` at `history` ends the hand, or NaN.
    static double terminal(std::array<int, 2> deal, int history, int action) {
        const double showdown = deal[0] > deal[1] ? 1.0 : -1.0;
        if (history == 1 && action == 0) return showdown;
        if (history == 2) return action == 0 ? 1.0 : 2.0 * showdown;
        if (history == 3) return action == 0 ? -1.0 : 2.0 * showdown;
        return std::numeric_limits<double>::quiet_NaN();
    }

    static int next(int history, int action) { return history == 0 ? 1 + action : 3; }

    std::array<double, 2> current(std::size_t info) const {
        std::array<double, 2> strategy{};
        regret_match(std::span<const double>(regret_[info]), 2, 1, std::span<double>(strategy));
        return strategy;
    }

    double walk(std::array<int, 2> deal, int history, int traverser, double own, double opp) {
        const int player = actor(history);
        const auto info = static_cast<std::size_t>(deal[static_cast<std::size_t>(player)] * 4 + history);
        const std::array<double, 2> sigma = current(info);
        std::array<double, 2> values{};
        double value = 0.0;
        for (int a = 0; a < 2; ++a) {
            const auto i = static_cast<std::size_t>(a);
            const double end = terminal(deal, history, a);
            if (!std::isnan(end)) {
                values[i] = traverser == 0 ? end : -end;
            } else if (player == traverser) {
                values[i] = walk(deal, next(history, a), traverser, own * sigma[i], opp);
            } else {
                values[i] = walk(deal, next(history, a), traverser, own, opp * sigma[i]);
            }
            value += sigma[i] * values[i];
        }
        if (player == traverser) {
            for (std::size_t a = 0; a < 2; ++a) {
                delta_regret_[info][a] += opp * (values[a] - value);
                delta_sum_[info][a] += own * sigma[a];
            }
        }
        return value;
    }

    double expected(std::array<int, 2> deal, int history) const {
        const int player = actor(history);
        double value = 0.0;
        for (int a = 0; a < 2; ++a) {
            const double p = average_bet(deal[static_cast<std::size_t>(player)], history);
            const double weight = a == 1 ? p : 1.0 - p;
            const double end = terminal(deal, history, a);
            value += weight * (std::isnan(end) ? expected(deal, next(history, a)) : end);
        }
        return value;
    }

    std::array<std::array<double, 2>, 12> regret_{};
    std::array<std::array<double, 2>, 12> sum_{};
    std::array<std::array<double, 2>, 12> delta_regret_{};
    std::array<std::array<double, 2>, 12> delta_sum_{};
};

TEST(Dcfr, DiscountsFollowThePaperSchedule) {
    const DcfrParams params;
    const DcfrDiscount first = dcfr_discount(params, 1);
    EXPECT_DOUBLE_EQ(first.positive, 0.0);
    EXPECT_DOUBLE_EQ(first.negative, 0.5);
    EXPECT_DOUBLE_EQ(first.strategy, 0.0);
    // Iteration 5 scales the sums by the factors for t = 4.
    const DcfrDiscount fifth = dcfr_discount(params, 5);
    EXPECT_DOUBLE_EQ(fifth.positive, 8.0 / 9.0);
    EXPECT_DOUBLE_EQ(fifth.negative, 0.5);
    EXPECT_DOUBLE_EQ(fifth.strategy, 0.64);

    const std::vector<float> regrets{3.0f, -1.0f, 1.0f, -2.0f, 0.0f, -4.0f};  // 3 actions x 2 hands
    std::vector<float> strategy(6);
    regret_match(std::span<const float>(regrets), 3, 2, std::span<float>(strategy));
    EXPECT_FLOAT_EQ(strategy[0], 0.75f);
    EXPECT_FLOAT_EQ(strategy[2], 0.25f);
    EXPECT_FLOAT_EQ(strategy[4], 0.0f);
    for (std::size_t a = 0; a < 3; ++a) EXPECT_FLOAT_EQ(strategy[a * 2 + 1], 1.0f / 3.0f);  // All <= 0: uniform
}

TEST(Dcfr, KuhnPokerValueIsMinusOneEighteenth) {
    Kuhn kuhn;
    const DcfrParams params;
    for (int t = 1; t <= 20000; ++t) kuhn.iterate(params, t);
    EXPECT_NEAR(kuhn.value(), -1.0 / 18.0, 0.002);
    // Player 2's equilibrium strategy is unique: call a bet with Q 1/3 of the time,
    // and bet J (a bluff) 1/3 of the time after a check.
    EXPECT_NEAR(kuhn.average_bet(1, 2), 1.0 / 3.0, 0.03);
    EXPECT_NEAR(kuhn.average_bet(0, 1), 1.0 / 3.0, 0.03);
    // And the pure parts: K always bets or calls, J never calls, Q never bets after a check.
    EXPECT_GT(kuhn.average_bet(2, 1), 0.97);
    EXPECT_GT(kuhn.average_bet(2, 2), 0.97);
    EXPECT_LT(kuhn.average_bet(0, 2), 0.03);
    EXPECT_LT(kuhn.average_bet(1, 1), 0.03);
}

// --- Betting tree ------------------------------------------------------------

BettingRoot river_root(int pot, int stack) {
    BettingRoot root;
    root.board = cards({"Kh", "9d", "5c", "3s", "2h"});
    root.pot = pot;
    root.effective_stack = stack;
    root.min_bet = 20;
    return root;
}

TEST(BettingTree, RiverDefaultSizesFollowTheRules) {
    const BettingTree tree = build_betting_tree(river_root(300, 1000), river_default_abstraction());
    // Root: check, then the ladder 75 / 150 / 225 / 300 and all-in 1000.
    const std::span<const TreeAction> root = tree.actions_of(0);
    ASSERT_EQ(root.size(), 6u);
    EXPECT_EQ(root[0].type, ActionType::Check);
    const std::array<int, 5> totals{75, 150, 225, 300, 1000};
    for (std::size_t i = 0; i < totals.size(); ++i) EXPECT_EQ(root[i + 1].to, totals[i]);
    EXPECT_EQ(root[5].type, ActionType::AllIn);
    EXPECT_DOUBLE_EQ(root[2].pot_fraction, 0.5);

    // Facing the pot bet of 300: pot after calling is 900, so a pot raise is to 1200,
    // past the stack: only fold, call and all-in remain.
    const int facing_pot = tree.child(0, 4);
    const std::span<const TreeAction> versus_pot = tree.actions_of(facing_pot);
    ASSERT_EQ(versus_pot.size(), 3u);
    EXPECT_EQ(versus_pot[0].type, ActionType::Fold);
    EXPECT_EQ(versus_pot[1].type, ActionType::Call);
    EXPECT_EQ(versus_pot[1].adds, 300);
    EXPECT_EQ(versus_pot[2].type, ActionType::AllIn);

    // After a check, IP's 150% bet (450) commits 45% of the stack and stays; facing the
    // 75 bet, IP's pot raise is to 75 + 450 = 525, 52.5% of the stack, so it stays too.
    const int ip = tree.child(0, 0);
    const std::span<const TreeAction> ip_actions = tree.actions_of(ip);
    ASSERT_EQ(ip_actions.size(), 5u);
    EXPECT_EQ(ip_actions[1].to, 99);
    EXPECT_EQ(ip_actions[2].to, 225);
    EXPECT_EQ(ip_actions[3].to, 450);
    const std::span<const TreeAction> versus_small = tree.actions_of(tree.child(0, 1));
    ASSERT_EQ(versus_small.size(), 4u);
    EXPECT_EQ(versus_small[2].type, ActionType::Raise);
    EXPECT_EQ(versus_small[2].to, 525);

    // Every bet obeys the minimum raise and the stack, and chips add up.
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const TreeNode& node = tree.nodes[i];
        EXPECT_EQ(node.pot, 300 + node.invested[0] + node.invested[1]);
        EXPECT_LE(node.street_bet[0], 1000);
        EXPECT_LE(node.street_bet[1], 1000);
        if (node.kind == NodeKind::Showdown) {
            EXPECT_EQ(node.invested[0], node.invested[1]);
        }
        if (node.kind != NodeKind::Decision) continue;
        EXPECT_LE(node.raises, 2);
        int previous = -1;
        for (const TreeAction& action : tree.actions_of(static_cast<int>(i))) {
            if (action.type == ActionType::Fold || action.type == ActionType::Check || action.type == ActionType::Call) {
                continue;
            }
            EXPECT_GT(action.to, previous);  // Sorted, no duplicates
            previous = action.to;
            const int current = std::max(node.street_bet[0], node.street_bet[1]);
            if (action.type != ActionType::AllIn) {
                EXPECT_GE(action.to - current, 20);
            }
        }
    }
}

TEST(BettingTree, MinimumRaiseAndClosingChecks) {
    BettingRoot root = river_root(200, 2000);
    root.street_bet = {0, 100};  // OOP checked, IP bet 100
    BetAbstraction abstraction;
    abstraction.streets[2].raises = {0.1};  // 0.1 x 400 = 40, below the minimum raise of 100
    abstraction.add_all_in = false;
    const BettingTree facing = build_betting_tree(root, abstraction);
    const std::span<const TreeAction> actions = facing.actions_of(0);
    ASSERT_EQ(actions.size(), 3u);
    EXPECT_EQ(actions[2].to, 200);  // Min-raise: 100 more
    EXPECT_DOUBLE_EQ(actions[2].pot_fraction, 0.25);

    // IP to act with equal bets means OOP checked: IP's check ends the hand.
    BettingRoot checked = river_root(200, 2000);
    checked.to_act = 1;
    const BettingTree tree = build_betting_tree(checked, BetAbstraction{});
    EXPECT_EQ(tree.nodes[static_cast<std::size_t>(tree.child(0, 0))].kind, NodeKind::Showdown);
    EXPECT_THROW(build_betting_tree(river_root(200, 0), BetAbstraction{}), std::invalid_argument);

    // A size far past the stack is all-in, even where size x pot overflows a 32-bit long.
    BetAbstraction huge;
    huge.streets[2].bets = {1e12};
    huge.add_all_in = false;
    const BettingTree shove = build_betting_tree(river_root(200, 1000), huge);
    ASSERT_EQ(shove.actions_of(0).size(), 2u);
    EXPECT_EQ(shove.actions_of(0)[1].type, ActionType::AllIn);
    EXPECT_EQ(shove.actions_of(0)[1].to, 1000);
}

TEST(BettingTree, TurnTreeDealsEveryRiver) {
    BettingRoot root = river_root(200, 300);
    root.board.pop_back();
    const BettingTree tree = build_betting_tree(root, river_default_abstraction());
    int chance = 0;
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        if (tree.nodes[i].kind != NodeKind::Chance) continue;
        ++chance;
        EXPECT_EQ(tree.nodes[i].child_count, 48);
        for (const int child : tree.children_of(static_cast<int>(i))) {
            EXPECT_EQ(tree.boards[static_cast<std::size_t>(tree.nodes[static_cast<std::size_t>(child)].board)].size, 5);
        }
    }
    EXPECT_GT(chance, 0);
    EXPECT_EQ(tree.boards.size(), 49u);  // The turn board and one per river card, shared
}

// --- Showdown ----------------------------------------------------------------

TEST(Showdown, MatchesBruteForceWithCardRemoval) {
    std::mt19937_64 rng(20260611);
    for (int trial = 0; trial < 12; ++trial) {
        // A random board, and random combos for each player: some hold board cards.
        std::vector<int> deck(kDeckSize);
        for (int i = 0; i < kDeckSize; ++i) deck[static_cast<std::size_t>(i)] = i;
        for (int i = kDeckSize - 1; i > 0; --i) {
            std::swap(deck[static_cast<std::size_t>(i)], deck[static_cast<std::size_t>(rng() % static_cast<std::uint64_t>(i + 1))]);
        }
        std::vector<Card> board;
        for (int i = 0; i < 5; ++i) board.emplace_back(deck[static_cast<std::size_t>(i)]);

        std::array<std::vector<int>, 2> combos;
        for (int combo = 0; combo < kComboCount; ++combo) {
            for (std::size_t p = 0; p < 2; ++p) {
                if (unit(rng) < 0.3) combos[p].push_back(combo);
            }
        }
        const MatchupHands hands(combos[0], combos[1]);
        const ShowdownTable table(hands, board);
        for (int p = 0; p < 2; ++p) {
            const auto me = static_cast<std::size_t>(p);
            std::vector<double> reach(combos[1 - me].size());
            for (double& r : reach) r = unit(rng) < 0.1 ? 0.0 : unit(rng);
            double scale = 0.0;
            for (const double r : reach) scale += r;

            std::vector<double> fast(combos[me].size());
            std::vector<double> slow(combos[me].size());
            table.values(hands, p, reach, 120.0, 20.0, -80.0, fast);
            showdown_values_reference(hands, board, p, reach, 120.0, 20.0, -80.0, slow);
            for (std::size_t h = 0; h < fast.size(); ++h) ASSERT_NEAR(fast[h], slow[h], 1e-9 * 120.0 * scale);

            fold_values(hands, p, reach, -35.0, fast);
            fold_values_reference(hands, p, reach, -35.0, slow);
            for (std::size_t h = 0; h < fast.size(); ++h) ASSERT_NEAR(fast[h], slow[h], 1e-9 * 35.0 * scale);
        }
    }
}

// --- River solver ------------------------------------------------------------

WeightedRange range_of(const char* text) { return WeightedRange::from_hand_range(HandRange::parse(text)); }

WeightedRange combos_of(std::initializer_list<std::array<const char*, 2>> holes) {
    WeightedRange range;
    for (const auto& hole : holes) range.set(combo_index(card(hole[0]), card(hole[1])), 1.0f);
    return range;
}

// The bettor holds three sets of kings and sixteen J-high bluffs; the caller holds
// 33, which beats every bluff and loses to every set. After OOP checks, IP can bet
// `size` pots or check, and OOP can only call or fold. At equilibrium the bettor
// bluffs so the caller is indifferent: b/(v+b) * (P + sP) = v/(v+b) * sP gives a
// bluff share of s/(1+2s); the caller calls so a bluff is indifferent:
// (1-c) P = c sP gives c = 1/(1+s).
void check_polarized_toy(double size) {
    SolveSpot spot;
    spot.board = cards({"Ks", "Qd", "7h", "4c", "2s"});
    spot.pot = 100;
    spot.effective_stack = 1000;
    spot.to_act = 1;
    spot.ranges[1] = combos_of({{"Kh", "Kd"}, {"Kh", "Kc"}, {"Kd", "Kc"}});
    for (const char* jack : {"Jc", "Jd", "Jh", "Js"}) {
        for (const char* nine : {"9c", "9d", "9h", "9s"}) spot.ranges[1].set(combo_index(card(jack), card(nine)), 1.0f);
    }
    spot.ranges[0] = range_of("33");

    BetAbstraction abstraction;
    abstraction.root_sizes = {size};
    abstraction.add_all_in = false;
    abstraction.raise_cap = 0;
    PostflopSolver solver(spot, abstraction);
    ASSERT_EQ(solver.tree().actions_of(0).size(), 2u);
    solver.run(2000);

    const std::span<const int> ip = solver.hands(1);
    ASSERT_EQ(ip.size(), 19u);
    const std::vector<float> root = solver.average_strategy(0);  // [check, bet][hand]
    double value_bets = 0.0;
    double bluffs = 0.0;
    for (std::size_t h = 0; h < ip.size(); ++h) {
        const double bet = root[ip.size() + h];
        if (combo_cards(ip[h])[0].rank() == Rank::King) {
            EXPECT_GE(bet, 0.98) << "value hand " << h;
            value_bets += bet;
        } else {
            bluffs += bet;
        }
    }
    EXPECT_NEAR(bluffs / (bluffs + value_bets), size / (1.0 + 2.0 * size), 0.02);

    const int facing = solver.tree().child(0, 1);
    const std::vector<float> response = solver.average_strategy(facing);  // [fold, call][hand]
    const std::size_t oop = solver.hands(0).size();
    ASSERT_EQ(oop, 6u);
    double calls = 0.0;
    for (std::size_t h = 0; h < oop; ++h) calls += response[oop + h];
    EXPECT_NEAR(calls / static_cast<double>(oop), 1.0 / (1.0 + size), 0.02);
    EXPECT_LT(solver.exploitability_pct_pot(), 0.5);
}

TEST(RiverToy, PolarizedVsBluffCatcher) {
    // No hole cards make a straight or a flush on this board, so 33 beats J9 and
    // loses to KK (a set) with nothing else possible.
    const std::vector<Card> board = cards({"Ks", "Qd", "7h", "4c", "2s"});
    const CardMask dead = card_mask(board);
    for (int combo = 0; combo < kComboCount; ++combo) {
        if ((combo_mask(combo) & dead) != 0) continue;
        const HandCategory category = hand_category(rank_of(combo, board));
        ASSERT_NE(category, HandCategory::Straight);
        ASSERT_NE(category, HandCategory::Flush);
        ASSERT_NE(category, HandCategory::StraightFlush);
    }
    for (const double size : {0.5, 1.0}) {
        SCOPED_TRACE(size);
        check_polarized_toy(size);
    }
}

TEST(RiverSolver, SymmetricCheckDownSplitsThePot) {
    SolveSpot spot;
    spot.board = cards({"Kh", "9d", "5c", "3s", "2h"});
    spot.pot = 300;
    spot.effective_stack = 1000;
    spot.ranges[0] = range_of("22+, A2s+, KTs+, QJs, T9s, ATo+, KQo");
    spot.ranges[1] = spot.ranges[0];
    BetAbstraction checks_only;
    checks_only.add_all_in = false;
    PostflopSolver solver(spot, checks_only);
    solver.run(5);

    const RootSolution root = solver.root();
    ASSERT_EQ(root.actions.size(), 1u);
    double share = 0.0;
    double ev = 0.0;
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        share += root.share[h];
        ev += root.share[h] * root.hand_ev[h];
    }
    EXPECT_NEAR(share, 1.0, 1e-9);
    EXPECT_NEAR(ev, 150.0, 1e-3);
    EXPECT_NEAR(solver.exploitability_pct_pot(), 0.0, 1e-9);
}

// BTN against BB after a single raise, on a dry river: about 250-300 combos each.
SolveSpot realistic_spot() {
    SolveSpot spot;
    spot.board = cards({"Kh", "9d", "5c", "3s", "2h"});
    spot.pot = 300;
    spot.effective_stack = 1000;
    spot.min_bet = 20;
    spot.ranges[0] = range_of(
        "22-JJ, A2s-AJs, K5s+, Q8s+, J8s+, T8s+, 97s+, 86s+, 74s+, 64s+, 54s, ATo-AQo, KTo+, QTo+, JTo");
    spot.ranges[1] = range_of("22+, A2s+, K9s+, QTs+, JTs, T9s, 98s, 87s, 76s, ATo+, KTo+, QJo");
    return spot;
}

TEST(RiverSolver, ExploitabilityFalls) {
    const SolveSpot spot = realistic_spot();
    PostflopSolver solver(spot, river_default_abstraction());
    EXPECT_GE(solver.hands(0).size(), 250u);
    EXPECT_LE(solver.hands(0).size(), 300u);
    EXPECT_GE(solver.hands(1).size(), 200u);
    EXPECT_LE(solver.hands(1).size(), 300u);

    solver.run(30);
    const double early = solver.exploitability_pct_pot();
    solver.run(270);
    const double late = solver.exploitability_pct_pot();
    EXPECT_EQ(solver.iterations(), 300);
    EXPECT_LT(late, 1.0);
    EXPECT_LT(late, early);
    EXPECT_GE(late, -1e-9);  // A best response can never do worse than the equilibrium value

    // Root EVs: every hand's average is a mix of its action EVs, within the pot's bounds.
    const RootSolution root = solver.root();
    ASSERT_EQ(root.freq.size(), root.actions.size());
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        double freq = 0.0;
        float best = -1e9f;
        for (std::size_t a = 0; a < root.actions.size(); ++a) {
            freq += root.freq[a][h];
            best = std::max(best, root.ev[a][h]);
        }
        EXPECT_NEAR(freq, 1.0, 1e-5);
        EXPECT_LE(root.hand_ev[h], best + 1e-3f);
        EXPECT_GE(root.hand_ev[h], -1000.0f);
        EXPECT_LE(root.hand_ev[h], 1300.0f);
    }
}

// ev_best lets the root player best-respond after its root action, so it bounds ev
// from above, matches it for actions a converged hand actually takes, and its
// per-hand maximum is the best-response value.
TEST(RiverSolver, BestContinuationEvBoundsAverageEv) {
    for (const int to_act : {0, 1}) {
        SCOPED_TRACE(to_act);
        SolveSpot spot = realistic_spot();
        spot.to_act = to_act;
        PostflopSolver solver(spot, river_default_abstraction());
        solver.run(600);
        ASSERT_LT(solver.exploitability_pct_pot(), 0.3);

        const RootSolution root = solver.root();
        ASSERT_EQ(root.ev_best.size(), root.actions.size());
        double br = 0.0;
        double gap = 0.0;  // Share-weighted ev_best - ev over actions taken at least 5%
        double gap_mass = 0.0;
        for (std::size_t h = 0; h < root.hands.size(); ++h) {
            float best = -1e9f;
            for (std::size_t a = 0; a < root.actions.size(); ++a) {
                ASSERT_EQ(root.ev_best[a].size(), root.hands.size());
                EXPECT_GE(root.ev_best[a][h], root.ev[a][h] - 1e-3f) << "hand " << h << " action " << a;
                best = std::max(best, root.ev_best[a][h]);
                if (root.freq[a][h] >= 0.05f) {
                    EXPECT_NEAR(root.ev_best[a][h], root.ev[a][h], 1.0) << "hand " << h << " action " << a;
                    gap += root.share[h] * (root.ev_best[a][h] - root.ev[a][h]);
                    gap_mass += root.share[h];
                }
            }
            br += root.share[h] * best;
            // A hand's average strategy is near a best response, so its EV is close to the maximum.
            EXPECT_LE(root.hand_ev[h], best + 1e-3f);
        }
        EXPECT_LT(gap / gap_mass, 0.1);  // Measured 0.02 chips in a pot of 300
        const std::array<double, 2> values = solver.best_response_values();
        EXPECT_NEAR(br, values[static_cast<std::size_t>(root.player)], 1e-2);
    }
}

// Whether no combo of the other range `combo` can face beats it (`nuts`), or none
// is worse than it (air: calling can at best split the pot with the same hand).
bool beats_all(int combo, const WeightedRange& other, const std::vector<Card>& board, bool nuts) {
    const HandRank mine = rank_of(combo, board);
    const CardMask blocked = combo_mask(combo) | card_mask(board);
    for (int o = 0; o < kComboCount; ++o) {
        if (other.weight(o) <= 0.0f || (combo_mask(o) & blocked) != 0) continue;
        const HandRank theirs = rank_of(o, board);
        if (nuts ? theirs < mine : theirs > mine) return false;
    }
    return true;
}

TEST(RiverSolver, NutsNeverFoldAndAirNeverCalls) {
    const SolveSpot spot = realistic_spot();
    PostflopSolver solver(spot, river_default_abstraction());
    solver.run(200);
    const BettingTree& tree = solver.tree();
    const std::vector<float> root = solver.average_strategy(0);
    const std::size_t oop_hands = solver.hands(0).size();

    int nut_checks = 0;
    int air_checks = 0;
    // Responses to a bet, with the responder's reach still high enough to matter:
    // IP facing each of OOP's root bets, and OOP facing IP's bets after checking.
    const auto check_responses = [&](int node, int player, const std::vector<double>& reach) {
        const std::span<const int> hands = solver.hands(player);
        const std::vector<float> strategy = solver.average_strategy(node);
        const std::span<const TreeAction> actions = tree.actions_of(node);
        ASSERT_EQ(actions[0].type, ActionType::Fold);
        ASSERT_EQ(actions[1].type, ActionType::Call);
        const WeightedRange& other = spot.ranges[static_cast<std::size_t>(1 - player)];
        for (std::size_t h = 0; h < hands.size(); ++h) {
            if (reach[h] < 0.05) continue;
            if (beats_all(hands[h], other, spot.board, true)) {
                ++nut_checks;
                EXPECT_LT(strategy[h], 0.01) << "nut hand folds, node " << node;
            }
            if (beats_all(hands[h], other, spot.board, false)) {
                ++air_checks;
                EXPECT_LT(strategy[hands.size() + h], 0.01) << "air calls, node " << node;
            }
        }
    };

    const std::span<const TreeAction> root_actions = tree.actions_of(0);
    const std::vector<double> full_ip(solver.hands(1).size(), 1.0);
    for (std::size_t a = 1; a < root_actions.size(); ++a) check_responses(tree.child(0, static_cast<int>(a)), 1, full_ip);

    std::vector<double> checked(oop_hands);
    for (std::size_t h = 0; h < oop_hands; ++h) checked[h] = root[h];
    const int ip_node = tree.child(0, 0);
    const std::span<const TreeAction> ip_actions = tree.actions_of(ip_node);
    for (std::size_t a = 1; a < ip_actions.size(); ++a) check_responses(tree.child(ip_node, static_cast<int>(a)), 0, checked);

    EXPECT_GT(nut_checks, 0);
    EXPECT_GT(air_checks, 0);
}

TEST(RiverSolver, EstimateBytesBoundsAllocation) {
    const SolveSpot spot = realistic_spot();
    const BetAbstraction abstraction = river_default_abstraction();
    const std::size_t estimate = PostflopSolver::estimate_bytes(spot, abstraction);
    const PostflopSolver solver(spot, abstraction);
    const std::size_t actual = solver.allocated_bytes();
    EXPECT_GE(estimate, actual);
    EXPECT_LE(static_cast<double>(estimate), 1.2 * static_cast<double>(actual));

    // Regrets and strategy sums dominate: 8 bytes per (decision action, hand).
    std::size_t slots = 0;
    for (std::size_t i = 0; i < solver.tree().nodes.size(); ++i) {
        const TreeNode& node = solver.tree().nodes[i];
        if (node.kind == NodeKind::Decision) {
            slots += static_cast<std::size_t>(node.child_count) * solver.hands(node.player).size();
        }
    }
    EXPECT_GE(actual, 8 * slots);

    SolverLimits tight;
    tight.memory_bytes = estimate - 1;
    EXPECT_THROW(PostflopSolver(spot, abstraction, tight), SolverMemoryError);
}

}  // namespace

namespace {

TEST(RiverSolver, StopTokenHaltsBeforeTheNextIteration) {
    PostflopSolver solver(realistic_spot(), river_default_abstraction());
    std::stop_source stop;
    solver.run(3, stop.get_token());
    EXPECT_EQ(solver.iterations(), 3);
    stop.request_stop();
    solver.run(100, stop.get_token());
    EXPECT_EQ(solver.iterations(), 3);
}

// Chance nodes: checked down from the turn, each hand's EV is the pot times its
// equity against the other range over every river, with card removal.
TEST(TurnSolver, CheckDownEvMatchesExactEquity) {
    SolveSpot spot;
    spot.board = cards({"Kh", "9d", "5c", "3s"});
    spot.pot = 200;
    spot.effective_stack = 400;
    spot.ranges[0] = range_of("AA, 99, 76s, A5s, QJo");
    spot.ranges[1] = range_of("KK, KQs, 44, T8s, A4o");
    spot.ranges[1].set(combo_index(card("Ah"), card("Ad")), 0.5f);  // Shares cards with player 0's aces
    BetAbstraction checks_only;
    checks_only.add_all_in = false;
    PostflopSolver solver(spot, checks_only);
    solver.run(1);
    const RootSolution root = solver.root();

    const CardMask board = card_mask(spot.board);
    for (std::size_t h = 0; h < root.hands.size(); ++h) {
        const int mine = root.hands[h];
        double weight = 0.0;
        double share = 0.0;
        for (int o = 0; o < kComboCount; ++o) {
            const float w = spot.ranges[1].weight(o);
            if (w <= 0.0f || (combo_mask(o) & (board | combo_mask(mine))) != 0) continue;
            double wins = 0.0;
            int rivers = 0;
            for (int river = 0; river < kDeckSize; ++river) {
                if (((board | combo_mask(mine) | combo_mask(o)) & (CardMask{1} << river)) != 0) continue;
                std::vector<Card> full = spot.board;
                full.emplace_back(river);
                const HandRank a = rank_of(mine, full);
                const HandRank b = rank_of(o, full);
                wins += a < b ? 1.0 : a == b ? 0.5 : 0.0;
                ++rivers;
            }
            weight += w;
            share += w * wins / rivers;
        }
        EXPECT_NEAR(root.hand_ev[h], 200.0 * share / weight, 1e-3) << "hand " << h;
    }
    EXPECT_NEAR(solver.exploitability_pct_pot(), 0.0, 1e-9);
}

TEST(TurnSolver, SmallTreeConverges) {
    SolveSpot spot;
    spot.board = cards({"Kh", "9d", "5c", "3s"});
    spot.pot = 200;
    spot.effective_stack = 400;
    spot.min_bet = 20;
    spot.ranges[0] = range_of("AA, 99, 76s, A5s, QJo, JTs");
    spot.ranges[1] = range_of("KK, KQs, 44, T8s, A4o, 87s");
    BetAbstraction abstraction;
    abstraction.streets[1].bets = {0.5};
    abstraction.streets[2].bets = {0.5};
    PostflopSolver solver(spot, abstraction);
    solver.run(10);
    const double early = solver.exploitability_pct_pot();
    solver.run(50);
    const double late = solver.exploitability_pct_pot();
    EXPECT_LT(late, early);
    EXPECT_LT(late, 1.0);
    EXPECT_GE(late, -1e-9);
}

}  // namespace
