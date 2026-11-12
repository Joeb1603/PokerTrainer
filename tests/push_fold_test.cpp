#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "range/combos.h"
#include "range/preflop_matrix.h"
#include "strategy/preflop_charts.h"
#include "strategy/push_fold.h"

using namespace poker;

namespace {

HandClass hc(const char* text) { return HandClass::parse(text).value(); }

constexpr std::array<Position, 5> kJammers{Position::UTG, Position::HJ, Position::CO, Position::BTN, Position::SB};

double jam_percent(Position jammer, int stack, int players = kMaxSeats) {
    return push_fold_jam_range(jammer, stack, players).value().percent();
}

double call_percent(Position caller, Position jammer, int stack, int players = kMaxSeats) {
    return push_fold_call_range(caller, jammer, stack, players).value().percent();
}

std::string spot(Position position, int stack) { return std::string(to_string(position)) + " " + std::to_string(stack) + " BB"; }

// An independent rebuild of the heads-up model: class weights with card removal
// from combo counts, equities from the preflop matrix.
struct HeadsUpModel {
    std::array<HandClass, kHandClassCount> by_index{};
    std::array<std::array<double, kHandClassCount>, kHandClassCount> given{};  // P(j | i)
    std::array<std::array<double, kHandClassCount>, kHandClassCount> equity{};

    HeadsUpModel() {
        for (const HandClass hand : hand_classes_by_strength()) by_index[static_cast<std::size_t>(hand.index())] = hand;
        for (std::size_t i = 0; i < by_index.size(); ++i) {
            for (std::size_t j = 0; j < by_index.size(); ++j) {
                int disjoint = 0;
                for (const std::uint16_t a : class_combos(by_index[i]).view()) {
                    for (const std::uint16_t b : class_combos(by_index[j]).view()) {
                        if ((combo_mask(a) & combo_mask(b)) == 0) ++disjoint;
                    }
                }
                // Each combo leaves 1225 two-card hands from the other 50 cards.
                given[i][j] = static_cast<double>(disjoint) / (by_index[i].combos() * 1225.0);
                equity[i][j] = class_vs_class_equity(by_index[i], by_index[j]);
            }
        }
    }

    // SB jam minus fold (-0.5), against the BB calling `calls`, at `stack` BB.
    double jam_gain(std::size_t i, const HandRange& calls, double stack) const {
        double called = 0.0;
        double won = 0.0;
        for (std::size_t j = 0; j < by_index.size(); ++j) {
            if (!calls.contains(by_index[j])) continue;
            called += given[i][j];
            won += given[i][j] * equity[i][j];
        }
        return (1.0 - called) * 1.0 + won * 2.0 * stack - called * stack + 0.5;
    }

    // BB call minus fold (-1), against the SB jamming `jams`.
    double call_gain(std::size_t j, const HandRange& jams, double stack) const {
        double jammed = 0.0;
        double won = 0.0;
        for (std::size_t i = 0; i < by_index.size(); ++i) {
            if (!jams.contains(by_index[i])) continue;
            jammed += given[j][i];
            won += given[j][i] * equity[j][i];
        }
        return won / jammed * 2.0 * stack - stack + 1.0;
    }
};

const HeadsUpModel& heads_up_model() {
    static const auto model = std::make_unique<HeadsUpModel>();
    return *model;
}

}  // namespace

TEST(PushFold, HeadsUpTenBbMatchesKnownNash) {
    // The commonly published heads-up equilibrium at 10 BB, no ante: the SB jams
    // about 58% and the BB calls about 37%.
    const double jam = jam_percent(Position::BTN, 10, 2);
    const double call = call_percent(Position::BB, Position::BTN, 10, 2);
    std::printf("heads-up 10 BB: SB jams %.2f%%, BB calls %.2f%% (%d iterations)\n", jam, call,
                push_fold_iterations());
    EXPECT_NEAR(jam, 58.0, 5.0);
    EXPECT_NEAR(call, 37.0, 5.0);
    // Six-handed, folded to the SB, is the same game.
    EXPECT_DOUBLE_EQ(jam, jam_percent(Position::SB, 10));
    EXPECT_DOUBLE_EQ(call, call_percent(Position::BB, Position::SB, 10));
    EXPECT_EQ(push_fold_iterations(), 2000);
}

TEST(PushFold, JamShareFallsAsStacksDeepen) {
    // No rise at all appeared in the generated tables, for jams or calls.
    for (const Position jammer : kJammers) {
        for (int stack = kPushFoldMinStackBb; stack < kPushFoldMaxStackBb; ++stack) {
            EXPECT_LE(jam_percent(jammer, stack + 1), jam_percent(jammer, stack)) << spot(jammer, stack);
        }
        for (int caller = static_cast<int>(jammer) + 1; caller <= static_cast<int>(Position::BB); ++caller) {
            const auto who = static_cast<Position>(caller);
            for (int stack = kPushFoldMinStackBb; stack < kPushFoldMaxStackBb; ++stack) {
                EXPECT_LE(call_percent(who, jammer, stack + 1), call_percent(who, jammer, stack))
                    << to_string(who) << " vs " << spot(jammer, stack);
            }
        }
    }
    EXPECT_GT(jam_percent(Position::SB, 3), jam_percent(Position::SB, 15) + 20.0);
}

TEST(PushFold, EarlierPositionsJamTighter) {
    for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
        for (std::size_t k = 0; k + 1 < kJammers.size(); ++k) {
            EXPECT_LT(jam_percent(kJammers[k], stack), jam_percent(kJammers[k + 1], stack)) << spot(kJammers[k], stack);
        }
        // The BB, closing the action at a discount, calls widest.
        for (const Position jammer : {Position::UTG, Position::HJ, Position::CO, Position::BTN}) {
            EXPECT_GE(call_percent(Position::BB, jammer, stack), call_percent(Position::SB, jammer, stack))
                << spot(jammer, stack);
            if (jammer != Position::BTN) {
                EXPECT_GE(call_percent(Position::SB, jammer, stack), call_percent(Position::BTN, jammer, stack))
                    << spot(jammer, stack);
            }
        }
        // Later jams are called wider.
        EXPECT_LT(call_percent(Position::BB, Position::UTG, stack), call_percent(Position::BB, Position::BTN, stack));
        // At 3 BB the SB's dead half blind gives the BB a better price against the
        // BTN (it calls 97% there, 93% against the SB), so the order starts at 4.
        if (stack >= 4) {
            EXPECT_LT(call_percent(Position::BB, Position::BTN, stack), call_percent(Position::BB, Position::SB, stack));
        }
    }
}

TEST(PushFold, CallRangesAreSubsetsOfStrongHands) {
    const HeadsUpModel& model = heads_up_model();
    for (const Position jammer : kJammers) {
        for (int caller = static_cast<int>(jammer) + 1; caller <= static_cast<int>(Position::BB); ++caller) {
            const auto who = static_cast<Position>(caller);
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                SCOPED_TRACE(std::string(to_string(who)) + " vs " + spot(jammer, stack));
                const HandRange calls = push_fold_call_range(who, jammer, stack).value();
                const HandRange jams = push_fold_jam_range(jammer, stack).value();
                EXPECT_TRUE(calls.contains(hc("AA")));
                EXPECT_TRUE(calls.contains(hc("KK")));
                EXPECT_TRUE(calls.contains(hc("AKs")));
                EXPECT_TRUE(jams.contains(hc("AA")));
                if (stack >= 6) {
                    EXPECT_FALSE(calls.contains(hc("72o")));
                    EXPECT_FALSE(calls.contains(hc("32o")));
                }
                // A call is a threshold on equity against the jam: every called class
                // has at least the equity of every folded one, up to the blur of the
                // published sets being thresholds of mixed averages.
                double weakest_call = 1.0;
                double best_fold = 0.0;
                for (std::size_t j = 0; j < model.by_index.size(); ++j) {
                    double jammed = 0.0;
                    double won = 0.0;
                    for (std::size_t i = 0; i < model.by_index.size(); ++i) {
                        if (!jams.contains(model.by_index[i])) continue;
                        jammed += model.given[j][i];
                        won += model.given[j][i] * model.equity[j][i];
                    }
                    const double equity = won / jammed;
                    if (calls.contains(model.by_index[j])) {
                        weakest_call = std::min(weakest_call, equity);
                    } else {
                        best_fold = std::max(best_fold, equity);
                    }
                }
                EXPECT_GT(weakest_call, best_fold - 0.02);
            }
        }
    }
}

TEST(PushFold, GeneratedTableMatchesARecomputation) {
    // Heads-up, each published set should be a best response to the other, except
    // for classes so close to indifferent that the threshold of a mixed average
    // can land either side.
    const HeadsUpModel& model = heads_up_model();
    for (const int stack : {5, 10, 15}) {
        SCOPED_TRACE(std::to_string(stack) + " BB");
        const HandRange jams = push_fold_jam_range(Position::SB, stack).value();
        const HandRange calls = push_fold_call_range(Position::BB, Position::SB, stack).value();
        int jam_mismatches = 0;
        int call_mismatches = 0;
        for (std::size_t k = 0; k < model.by_index.size(); ++k) {
            const HandClass hand = model.by_index[k];
            const double jam_gain = model.jam_gain(k, calls, stack);
            if ((jam_gain > 0.0) != jams.contains(hand)) {
                ++jam_mismatches;
                EXPECT_LT(std::abs(jam_gain), 0.05) << "SB " << hand.to_string();
            }
            const double call_gain = model.call_gain(k, jams, stack);
            if ((call_gain > 0.0) != calls.contains(hand)) {
                ++call_mismatches;
                EXPECT_LT(std::abs(call_gain), 0.05) << "BB " << hand.to_string();
            }
        }
        EXPECT_LE(jam_mismatches, 3);
        EXPECT_LE(call_mismatches, 3);
    }
}

TEST(PushFold, ResponsesAndNulloptCases) {
    // First in: jam or fold.
    EXPECT_EQ(push_fold_response(hc("AA"), Position::UTG, 10, std::nullopt), PreflopResponse::Raise);
    EXPECT_EQ(push_fold_response(hc("72o"), Position::UTG, 10, std::nullopt), PreflopResponse::Fold);
    EXPECT_EQ(push_fold_response(hc("K2s"), Position::SB, 10, std::nullopt), PreflopResponse::Raise);
    EXPECT_EQ(push_fold_response(hc("K2s"), Position::UTG, 10, std::nullopt), PreflopResponse::Fold);
    // Facing a lone jam: call or fold.
    EXPECT_EQ(push_fold_response(hc("AA"), Position::BB, 10, Position::UTG), PreflopResponse::Call);
    EXPECT_EQ(push_fold_response(hc("72o"), Position::BB, 10, Position::UTG), PreflopResponse::Fold);
    EXPECT_EQ(push_fold_response(hc("K9o"), Position::BB, 10, Position::SB), PreflopResponse::Call);
    // Heads-up the button plays the SB game.
    EXPECT_EQ(push_fold_jam_range(Position::BTN, 8, 2)->combos(), push_fold_jam_range(Position::SB, 8)->combos());
    EXPECT_EQ(push_fold_response(hc("K9o"), Position::BB, 10, Position::BTN, 2), PreflopResponse::Call);
    // Short-handed tables use the same tables.
    EXPECT_EQ(push_fold_jam_range(Position::CO, 9, 4)->combos(), push_fold_jam_range(Position::CO, 9)->combos());
    // Nullopt: stacks out of range, the BB first in, a hero not behind the shover.
    for (const int stack : {0, 2, 16, 100}) {
        EXPECT_FALSE(push_fold_response(hc("AA"), Position::BTN, stack, std::nullopt).has_value()) << stack;
        EXPECT_FALSE(push_fold_jam_range(Position::BTN, stack).has_value()) << stack;
        EXPECT_FALSE(push_fold_call_range(Position::BB, Position::BTN, stack).has_value()) << stack;
    }
    EXPECT_TRUE(push_fold_response(hc("AA"), Position::BTN, 3, std::nullopt).has_value());
    EXPECT_TRUE(push_fold_response(hc("AA"), Position::BTN, 15, std::nullopt).has_value());
    EXPECT_FALSE(push_fold_response(hc("AA"), Position::BB, 10, std::nullopt).has_value());
    EXPECT_FALSE(push_fold_response(hc("AA"), Position::UTG, 10, Position::CO).has_value());
    EXPECT_FALSE(push_fold_response(hc("AA"), Position::CO, 10, Position::CO).has_value());
    EXPECT_FALSE(push_fold_response(hc("AA"), Position::SB, 10, Position::BB).has_value());
    EXPECT_FALSE(push_fold_response(hc("AA"), Position::BTN, 10, Position::BB, 2).has_value());
}

TEST(PushFoldAntes, DeadMoneyPicksTheLargestLevelNotAbove) {
    const auto pct = [](double dead) { return push_fold_jam_range(Position::BTN, 10, kMaxSeats, dead)->percent(); };
    EXPECT_DOUBLE_EQ(pct(0.0), push_fold_jam_range(Position::BTN, 10)->percent());
    EXPECT_DOUBLE_EQ(pct(0.49), pct(0.0));
    EXPECT_DOUBLE_EQ(pct(0.5), pct(0.99));
    EXPECT_DOUBLE_EQ(pct(1.0), pct(1.49));
    EXPECT_DOUBLE_EQ(pct(1.5), pct(4.0));
    EXPECT_LT(pct(0.0), pct(0.5));
    EXPECT_LT(pct(0.5), pct(1.0));
    EXPECT_LT(pct(1.0), pct(1.5));
}

TEST(PushFoldAntes, MoreDeadMoneyWidensJamsAndCalls) {
    // Each table against the one below it: wider on average, and never much tighter
    // (fictitious play's rounding moves a few borderline classes).
    for (int level = 0; level < detail::kPushFoldAnteLevels; ++level) {
        const double lower = level == 0 ? 0.0 : detail::kPushFoldAnteDeadBb[level - 1];
        const double upper = detail::kPushFoldAnteDeadBb[level];
        double jam_gain = 0.0;
        double call_gain = 0.0;
        for (int jammer = 0; jammer < detail::kPushFoldJammerCount; ++jammer) {
            const auto from = static_cast<Position>(jammer);
            for (int stack = kPushFoldMinStackBb; stack <= kPushFoldMaxStackBb; ++stack) {
                const double a = push_fold_jam_range(from, stack, kMaxSeats, lower)->percent();
                const double b = push_fold_jam_range(from, stack, kMaxSeats, upper)->percent();
                EXPECT_GE(b, a - 2.0) << to_string(from) << " " << stack << " BB at " << upper;
                jam_gain += b - a;
                const double c = push_fold_call_range(Position::BB, from, stack, kMaxSeats, lower)->percent();
                const double d = push_fold_call_range(Position::BB, from, stack, kMaxSeats, upper)->percent();
                EXPECT_GE(d, c - 2.0) << "BB v " << to_string(from) << " " << stack << " BB at " << upper;
                call_gain += d - c;
            }
        }
        EXPECT_GT(jam_gain, 0.0);
        EXPECT_GT(call_gain, 0.0);
    }
}

TEST(PushFoldAntes, HeadsUpButtonUsesTheSmallBlindAnteTable) {
    EXPECT_EQ(push_fold_jam_range(Position::BTN, 8, 2, 1.0)->percent(),
              push_fold_jam_range(Position::SB, 8, kMaxSeats, 1.0)->percent());
}

TEST(PushFoldAntes, HeadsUpMatchesAnIndependentSolve) {
    // Heads-up the model is exact (one caller), so the tables were checked against a
    // separate solve: phevaluator Monte Carlo equities (40,000 boards a class pair),
    // its own fictitious play, combo card removal. Over 3-15 BB at 0, 0.5, 1 and
    // 1.5 BB of dead money the sets differed by at most one borderline class (0.9
    // pp). These are its numbers; at 10 BB with a 1 BB ante they also match the
    // published 71% shove and 53% call for a big-blind ante.
    struct Spot {
        int stack;
        double dead;
        double jam;
        double call;
    };
    for (const Spot s : {Spot{10, 0.0, 58.1, 37.6}, Spot{10, 0.5, 65.6, 46.6}, Spot{10, 1.0, 71.0, 53.2},
                         Spot{10, 1.5, 75.6, 59.3}, Spot{5, 1.0, 82.2, 82.2}, Spot{15, 1.0, 61.7, 40.6}}) {
        EXPECT_NEAR(push_fold_jam_range(Position::SB, s.stack, 2, s.dead)->percent(), s.jam, 1.0)
            << s.stack << " BB, " << s.dead << " dead";
        EXPECT_NEAR(push_fold_call_range(Position::BB, Position::SB, s.stack, 2, s.dead)->percent(), s.call, 1.0)
            << s.stack << " BB, " << s.dead << " dead";
    }
}
