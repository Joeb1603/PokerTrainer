#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "controller/local_game_controller.h"

using namespace poker;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::vector<json> read_json_lines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<json> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(json::parse(line));
    return lines;
}

Action passive(const LegalActions& legal) {
    return {legal.allows(ActionType::Check) ? ActionType::Check : ActionType::Call};
}

class Controller : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("poker_controller_") + info->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }

    ControllerOptions options(std::uint64_t seed = 42) const {
        ControllerOptions o;
        o.seed = seed;
        o.log_dir = dir_;
        o.instant_play = true;
        o.bot_rollouts = 60;
        o.advisor_rollouts = 60;
        return o;
    }

    fs::path hands() const { return dir_ / "hand_history.jsonl"; }
    fs::path sessions() const { return dir_ / "session_summary.jsonl"; }

    fs::path dir_;
};

// Plays until the hand is complete, with Hero choosing via `hero`.
template <typename Hero>
void play_hand_out(IGameController& c, Hero hero) {
    c.tick(0.0);
    for (int guard = 0; guard < 500; ++guard) {
        const TableSnapshot& s = c.snapshot();
        if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) return;
        if (s.phase == ControllerPhase::HeroToAct) {
            ASSERT_TRUE(c.submit_hero_action(hero(s)));
        }
        c.tick(0.0);
    }
    FAIL() << "hand did not finish";
}

void expect_no_hidden_text(const TableSnapshot& s) {
    for (const SeatSnapshot& seat : s.seats) EXPECT_EQ(seat.name.find("Shift:"), std::string::npos);
    for (const std::string& line : s.action_log) EXPECT_EQ(line.find("Shift:"), std::string::npos) << line;
    for (const PayoutLine& line : s.payouts) EXPECT_EQ(line.name.find("Shift:"), std::string::npos);
}

}  // namespace

// --- Hidden information ------------------------------------------------------

TEST_F(Controller, OpponentCardsHiddenUntilShowdownAndShifterNameNeverChanges) {
    LocalGameController c(options());
    int showdowns = 0;
    int fold_wins = 0;

    for (int hand = 0; hand < 60; ++hand) {
        for (int guard = 0; guard < 500; ++guard) {
            const TableSnapshot& s = c.snapshot();
            expect_no_hidden_text(s);
            EXPECT_EQ(s.seats[5].name, "Shifter");
            if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;

            for (int seat = 1; seat < kMaxSeats; ++seat) {
                EXPECT_FALSE(s.seats[seat].visible_hole.has_value()) << "seat " << seat << " shown mid-hand";
            }
            if (s.phase == ControllerPhase::HeroToAct) {
                EXPECT_TRUE(s.seats[kHeroSeat].visible_hole.has_value());
                ASSERT_TRUE(c.submit_hero_action(passive(s.legal)));
            }
            c.tick(0.0);
        }

        const TableSnapshot& s = c.snapshot();
        expect_no_hidden_text(s);
        ASSERT_FALSE(s.payouts.empty());
        if (s.showdown) {
            ++showdowns;
            int shown = 0;
            for (int seat = 0; seat < kMaxSeats; ++seat) {
                if (s.seats[seat].holds_cards) {
                    ++shown;
                    EXPECT_TRUE(s.seats[seat].visible_hole.has_value());
                    EXPECT_TRUE(s.seats[seat].shown_hand.has_value());
                } else if (seat != kHeroSeat) {
                    EXPECT_FALSE(s.seats[seat].visible_hole.has_value()) << "folded hand was shown";
                }
            }
            EXPECT_GE(shown, 2);
        } else {
            ++fold_wins;
            for (int seat = 1; seat < kMaxSeats; ++seat) {
                EXPECT_FALSE(s.seats[seat].visible_hole.has_value()) << "fold win exposed cards";
            }
        }
        if (s.phase == ControllerPhase::SessionOver) {
            c.new_session();  // Hero calls everything, so busts often
        } else {
            ASSERT_TRUE(c.next_hand());
        }
    }
    EXPECT_GT(showdowns, 0);
    EXPECT_GT(fold_wins, 0);
}

// --- Showdown results ----------------------------------------------------------

namespace {

std::string five_ranks(const std::array<Card, 5>& five) {
    std::string out;
    for (const Card& c : five) out += c.to_string()[0];
    return out;
}

}  // namespace

TEST_F(Controller, ShowdownResultsMarkSplitPotsAndKickers) {
    // Hero calls everything, so showdowns are common. Plays until both a split pot and
    // a win on kickers have appeared, checking every showdown along the way.
    LocalGameController c(options(7));
    int splits = 0;
    int kickers = 0;
    for (int hand = 0; hand < 600 && (splits == 0 || kickers == 0); ++hand) {
        play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });
        const TableSnapshot& s = c.snapshot();

        for (const SeatSnapshot& seat : s.seats) {
            EXPECT_EQ(seat.shown_five.has_value(), s.showdown && seat.shown_hand.has_value()) << seat.name;
        }
        for (const PayoutLine& line : s.payouts) {
            const auto me = std::find_if(s.seats.begin(), s.seats.end(),
                                         [&](const SeatSnapshot& seat) { return seat.name == line.name; });
            ASSERT_NE(me, s.seats.end());
            EXPECT_NE(std::find(s.action_log.begin(), s.action_log.end(), describe_payout(line)), s.action_log.end());
            if (!s.showdown) {
                EXPECT_FALSE(line.split);
                EXPECT_FALSE(line.kicker_note);
                continue;
            }
            ASSERT_TRUE(me->shown_five);
            if (line.split) {
                ++splits;
                // Someone else won chips with exactly the same five ranks.
                const bool partner = std::any_of(s.payouts.begin(), s.payouts.end(), [&](const PayoutLine& other) {
                    if (other.name == line.name) return false;
                    const auto them = std::find_if(s.seats.begin(), s.seats.end(),
                                                   [&](const SeatSnapshot& seat) { return seat.name == other.name; });
                    return them->shown_five && five_ranks(*them->shown_five) == five_ranks(*me->shown_five);
                });
                EXPECT_TRUE(partner) << describe_payout(line);
                EXPECT_NE(describe_payout(line).find("(split pot"), std::string::npos);
            }
            if (line.kicker_note) {
                ++kickers;
                // A beaten shown hand had the same name but different cards.
                const bool beaten = std::any_of(s.seats.begin(), s.seats.end(), [&](const SeatSnapshot& seat) {
                    return seat.name != line.name && seat.shown_hand == me->shown_hand && seat.shown_five &&
                           five_ranks(*seat.shown_five) != five_ranks(*me->shown_five);
                });
                EXPECT_TRUE(beaten) << describe_payout(line);
                EXPECT_NE(line.kicker_note->find(" kicker beats "), std::string::npos);
            }
        }

        if (s.phase == ControllerPhase::SessionOver) {
            c.new_session();
        } else {
            ASSERT_TRUE(c.next_hand());
        }
    }
    EXPECT_GT(splits, 0);
    EXPECT_GT(kickers, 0);

    // The hand log keeps its plain hand names.
    for (const json& line : read_json_lines(hands())) {
        for (const json& win : line["win"]) {
            if (!win.contains("hand")) continue;
            const std::string name = win["hand"].get<std::string>();
            EXPECT_EQ(name.find("kicker"), std::string::npos);
            EXPECT_EQ(name.find("split"), std::string::npos);
        }
    }
}

TEST(DescribePayout, AddsSplitAndKickerNotes) {
    PayoutLine line;
    line.name = "Hero";
    line.amount = 150;
    line.hand = "Ten-High Straight";
    EXPECT_EQ(describe_payout(line), "Hero wins 150 with Ten-High Straight");
    line.split = true;
    EXPECT_EQ(describe_payout(line), "Hero wins 150 with Ten-High Straight (split pot)");
    line.kicker_note = "Ace kicker beats Queen";
    EXPECT_EQ(describe_payout(line), "Hero wins 150 with Ten-High Straight (split pot; Ace kicker beats Queen)");
    PayoutLine fold_win;
    fold_win.name = "Nit";
    fold_win.amount = 30;
    EXPECT_EQ(describe_payout(fold_win), "Nit wins 30");
}

// --- Pacing ------------------------------------------------------------------

TEST_F(Controller, BotsActOnlyAfterTheDelayOneAtATime) {
    ControllerOptions o = options();
    o.instant_play = false;
    o.bot_delay_ms = 400.0;

    // Find a seed where bots act first, so Hero's turn does not hide the pacing.
    for (std::uint64_t seed = 1; seed < 50; ++seed) {
        o.seed = seed;
        LocalGameController c(o);
        if (c.snapshot().phase != ControllerPhase::BotToAct) continue;

        const std::size_t before = c.snapshot().action_log.size();
        c.tick(399.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before);
        c.tick(1.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before + 1);
        c.tick(100000.0);  // A stalled frame still moves one bot only
        EXPECT_LE(c.snapshot().action_log.size(), before + 2);
        return;
    }
    FAIL() << "no seed with a bot first to act";
}

TEST_F(Controller, InstantPlayRunsToHeroOrEndOfHandInOneTick) {
    LocalGameController c(options());
    c.tick(0.0);
    EXPECT_NE(c.snapshot().phase, ControllerPhase::BotToAct);
}

TEST_F(Controller, DelaySettingsAreAdjustable) {
    LocalGameController c(options());
    c.set_bot_delay_ms(150.0);
    c.set_instant_play(false);
    EXPECT_DOUBLE_EQ(c.bot_delay_ms(), 150.0);
    EXPECT_FALSE(c.instant_play());
}

TEST_F(Controller, HandStaysPausedUntilNextHand) {
    LocalGameController c(options());
    play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });
    ASSERT_EQ(c.snapshot().phase, ControllerPhase::HandComplete);

    const int hand = c.snapshot().hand_number;
    c.tick(100000.0);
    EXPECT_EQ(c.snapshot().phase, ControllerPhase::HandComplete);
    EXPECT_EQ(c.snapshot().hand_number, hand);
    EXPECT_FALSE(c.snapshot().payouts.empty());

    ASSERT_TRUE(c.next_hand());
    EXPECT_EQ(c.snapshot().hand_number, hand + 1);
    EXPECT_NE(c.snapshot().phase, ControllerPhase::HandComplete);
}

// --- Hero actions ------------------------------------------------------------

TEST_F(Controller, HeroActionsAreValidated) {
    LocalGameController c(options());
    EXPECT_FALSE(c.next_hand());

    c.tick(0.0);
    while (c.snapshot().phase == ControllerPhase::HandComplete) {
        ASSERT_TRUE(c.next_hand());
        c.tick(0.0);
    }
    ASSERT_EQ(c.snapshot().phase, ControllerPhase::HeroToAct);

    const TableSnapshot& s = c.snapshot();
    ASSERT_TRUE(s.hint.has_value());
    EXPECT_TRUE(s.legal.allows(s.hint->action.type));
    EXPECT_FALSE(c.submit_hero_action({ActionType::Raise, s.legal.max_raise_to + 1}));
    EXPECT_EQ(c.snapshot().phase, ControllerPhase::HeroToAct);

    EXPECT_TRUE(c.submit_hero_action({ActionType::Fold}));
    EXPECT_FALSE(c.submit_hero_action({ActionType::Fold}));  // No longer Hero's turn
    EXPECT_FALSE(c.snapshot().hint.has_value());
    EXPECT_EQ(c.snapshot().legal.mask, 0);
}

TEST_F(Controller, QuickBetsAreSizedFromThePotAfterCallingAndClamped) {
    LegalActions facing;
    facing.mask = 1u << static_cast<unsigned>(ActionType::Raise);
    facing.call_amount = 100;
    facing.min_raise_to = 200;
    facing.max_raise_to = 1000;
    // Pot 300 including a 100 bet: 400 after calling, so a pot raise goes to 100 + 400.
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 1.0, facing.max_raise_to), 500);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.5, facing.max_raise_to), 300);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.75, facing.max_raise_to), 400);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.0, facing.max_raise_to), 200);  // Min clamps up
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 10.0, facing.max_raise_to), 1000);  // Stack clamps down

    LegalActions unbet;
    unbet.mask = 1u << static_cast<unsigned>(ActionType::Bet);
    unbet.min_raise_to = 20;
    unbet.max_raise_to = 1000;
    EXPECT_EQ(quick_bet_to(unbet, 0, 200, 0.5, unbet.max_raise_to), 100);
    EXPECT_EQ(quick_bet_to(unbet, 0, 200, 1.0, unbet.max_raise_to), 200);

    EXPECT_EQ(quick_bet_to(LegalActions{}, 0, 200, 1.0, 0), 0);

    // A short opponent caps the size, but never below the legal minimum.
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 1.0, 350), 350);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 1.0, 150), 200);
}

TEST_F(Controller, BetPercentRoundTripsWithQuickBets) {
    LegalActions facing;
    facing.mask = 1u << static_cast<unsigned>(ActionType::Raise);
    facing.call_amount = 100;
    facing.min_raise_to = 200;
    facing.max_raise_to = 100000;
    LegalActions unbet;
    unbet.mask = 1u << static_cast<unsigned>(ActionType::Bet);
    unbet.min_raise_to = 20;
    unbet.max_raise_to = 100000;

    for (const int percent : {50, 75, 100, 150, 300}) {
        const double fraction = percent / 100.0;
        EXPECT_EQ(bet_pot_percent(facing, 100, 300, quick_bet_to(facing, 100, 300, fraction, facing.max_raise_to)), percent);
        EXPECT_EQ(bet_pot_percent(unbet, 0, 200, quick_bet_to(unbet, 0, 200, fraction, unbet.max_raise_to)), percent);
    }
    // Rounding of the chips stays within a percent on a small pot, and clamping is reported as it is.
    EXPECT_NEAR(bet_pot_percent(unbet, 0, 35, quick_bet_to(unbet, 0, 35, 0.75, unbet.max_raise_to)), 75, 1);
    EXPECT_EQ(bet_pot_percent(facing, 100, 300, quick_bet_to(facing, 100, 300, 0.0, facing.max_raise_to)), 25);  // Minimum raise
    EXPECT_EQ(bet_pot_percent(LegalActions{}, 0, 0, 100), 0);
}

// --- Current hand name -------------------------------------------------------

TEST_F(Controller, HeroHandNameShowsFromTheFlopUntilHeroFolds) {
    LocalGameController c(options());
    int named = 0;
    for (int hand = 0; hand < 40; ++hand) {
        for (int guard = 0; guard < 500; ++guard) {
            c.tick(0.0);
            const TableSnapshot& s = c.snapshot();
            const SeatSnapshot& hero = s.seats[kHeroSeat];
            if (s.board.size() < 3 || hero.status == SeatStatus::Folded) {
                EXPECT_FALSE(s.hero_hand.has_value());
            } else {
                ASSERT_TRUE(s.hero_hand.has_value());
                std::vector<Card> cards(hero.visible_hole->begin(), hero.visible_hole->end());
                cards.insert(cards.end(), s.board.begin(), s.board.end());
                EXPECT_EQ(*s.hero_hand, describe_made_hand(evaluate(cards)));
                ++named;
            }
            if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;
            if (s.phase == ControllerPhase::HeroToAct) {
                // Fold some flops, so a named hand is seen to clear.
                const bool fold = !s.board.empty() && hand % 3 == 0 && !s.legal.allows(ActionType::Check);
                ASSERT_TRUE(c.submit_hero_action(fold ? Action{ActionType::Fold} : passive(s.legal)));
            }
        }
        if (c.snapshot().phase == ControllerPhase::SessionOver) c.new_session();
        else ASSERT_TRUE(c.next_hand());
    }
    EXPECT_GT(named, 0);
}

// --- Play GTO ----------------------------------------------------------------

TEST_F(Controller, PlayingEveryHintLogsNoMistakes) {
    {
        LocalGameController c(options());
        for (int hand = 0; hand < 30; ++hand) {
            play_hand_out(c, [](const TableSnapshot& s) { return s.hint->action; });
            EXPECT_TRUE(c.snapshot().hand_mistakes.empty());
            if (c.snapshot().phase == ControllerPhase::SessionOver) c.new_session();
            else ASSERT_TRUE(c.next_hand());
        }
    }
    for (const json& line : read_json_lines(hands())) EXPECT_TRUE(line["hero_mistakes"].empty()) << line.dump();
}

// --- Early fold --------------------------------------------------------------

namespace {

std::vector<std::string> log_without_session_ids(const fs::path& path) {
    std::vector<std::string> out;
    for (json line : read_json_lines(path)) {
        line.erase("session_id");
        out.push_back(line.dump());
    }
    return out;
}

// Runs the bots until something other than a bot is up, whatever the delay.
void run_bots(IGameController& c) {
    for (int guard = 0; guard < 50 && c.snapshot().phase == ControllerPhase::BotToAct; ++guard) c.tick(1e9);
}

}  // namespace

TEST_F(Controller, EarlyFoldPlaysAndLogsExactlyLikeFoldingAtHerosTurn) {
    ControllerOptions base = options();
    base.instant_play = false;
    int compared = 0;

    for (std::uint64_t seed = 1; seed < 60 && compared < 5; ++seed) {
        ControllerOptions early = base;
        early.seed = seed;
        early.log_dir = dir_ / ("early" + std::to_string(seed));
        ControllerOptions manual = early;
        manual.log_dir = dir_ / ("manual" + std::to_string(seed));
        std::vector<std::string> early_actions;
        std::vector<std::string> manual_actions;
        {
            LocalGameController a(early);
            if (!a.snapshot().can_fold_early) continue;
            ASSERT_TRUE(a.set_early_fold(true));
            EXPECT_TRUE(a.snapshot().early_fold_armed);
            a.tick(0.0);
            if (a.snapshot().phase == ControllerPhase::HeroToAct) continue;  // A free check: tested below
            EXPECT_EQ(a.snapshot().seats[kHeroSeat].status, SeatStatus::Folded);
            EXPECT_FALSE(a.snapshot().early_fold_armed);
            run_bots(a);

            LocalGameController b(manual);
            run_bots(b);
            ASSERT_EQ(b.snapshot().phase, ControllerPhase::HeroToAct);
            ASSERT_TRUE(b.submit_hero_action({ActionType::Fold}));
            run_bots(b);

            early_actions = a.snapshot().action_log;
            manual_actions = b.snapshot().action_log;
        }
        EXPECT_EQ(early_actions, manual_actions);
        const auto early_log = log_without_session_ids(early.log_dir / "hand_history.jsonl");
        ASSERT_EQ(early_log.size(), 1u);
        EXPECT_EQ(early_log, log_without_session_ids(manual.log_dir / "hand_history.jsonl"));
        ++compared;
    }
    EXPECT_EQ(compared, 5);
}

TEST_F(Controller, EarlyFoldSkipsTheDelayOnlyWhileArmed) {
    ControllerOptions o = options();
    o.instant_play = false;
    o.bot_delay_ms = 400.0;

    for (std::uint64_t seed = 1; seed < 100; ++seed) {
        o.seed = seed;
        LocalGameController c(o);
        if (!c.snapshot().can_fold_early) continue;

        ASSERT_TRUE(c.set_early_fold(true));
        c.tick(0.0);
        EXPECT_DOUBLE_EQ(c.bot_delay_ms(), 400.0);
        EXPECT_FALSE(c.instant_play());
        // Needs bots still to act after Hero's fold, to see the delay come back.
        if (c.snapshot().seats[kHeroSeat].status != SeatStatus::Folded || c.snapshot().phase != ControllerPhase::BotToAct) continue;

        const std::size_t before = c.snapshot().action_log.size();
        c.tick(399.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before);
        c.tick(1.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before + 1);
        return;
    }
    FAIL() << "no seed with bots acting both before and after Hero";
}

TEST_F(Controller, CancellingAnEarlyFoldRestoresPacing) {
    ControllerOptions o = options();
    o.instant_play = false;
    o.bot_delay_ms = 400.0;

    for (std::uint64_t seed = 1; seed < 50; ++seed) {
        o.seed = seed;
        LocalGameController c(o);
        if (!c.snapshot().can_fold_early) continue;

        const std::size_t before = c.snapshot().action_log.size();
        ASSERT_TRUE(c.set_early_fold(true));
        ASSERT_TRUE(c.set_early_fold(false));
        EXPECT_FALSE(c.snapshot().early_fold_armed);
        c.tick(399.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before);
        c.tick(1.0);
        EXPECT_EQ(c.snapshot().action_log.size(), before + 1);

        run_bots(c);
        EXPECT_NE(c.snapshot().seats[kHeroSeat].status, SeatStatus::Folded);
        return;
    }
    FAIL() << "no seed with a bot first to act";
}

TEST_F(Controller, EarlyFoldBecomesADecisionWhenHeroCouldCheckForFree) {
    ControllerOptions o = options();
    o.instant_play = false;
    LocalGameController c(o);

    for (int step = 0; step < 20000; ++step) {
        const TableSnapshot& s = c.snapshot();
        if (s.phase == ControllerPhase::SessionOver) {
            c.new_session();
        } else if (s.phase == ControllerPhase::HandComplete) {
            ASSERT_TRUE(c.next_hand());
        } else if (s.phase == ControllerPhase::HeroToAct) {
            ASSERT_TRUE(c.submit_hero_action(passive(s.legal)));
        } else if (s.can_fold_early) {
            ASSERT_TRUE(c.set_early_fold(true));
            c.tick(0.0);
            const TableSnapshot& after = c.snapshot();
            EXPECT_FALSE(after.early_fold_armed);
            if (after.phase == ControllerPhase::HeroToAct) {
                EXPECT_TRUE(after.legal.allows(ActionType::Check));
                EXPECT_EQ(after.seats[kHeroSeat].status, SeatStatus::Active);
                return;
            }
        } else {
            c.tick(1e9);
        }
    }
    FAIL() << "never reached a free check with an early fold armed";
}

TEST_F(Controller, EarlyFoldIsRefusedAtHerosTurnAndDoesNotCarryOver) {
    ControllerOptions o = options();
    o.instant_play = false;

    for (std::uint64_t seed = 1; seed < 50; ++seed) {
        o.seed = seed;
        LocalGameController c(o);
        if (!c.snapshot().can_fold_early) {
            EXPECT_FALSE(c.set_early_fold(true));  // Hero is first to act
            EXPECT_FALSE(c.snapshot().early_fold_armed);
            continue;
        }

        ASSERT_TRUE(c.set_early_fold(true));
        c.new_session();
        EXPECT_FALSE(c.snapshot().early_fold_armed);

        for (int hand = 0; hand < 3; ++hand) {
            if (c.snapshot().can_fold_early) {
                ASSERT_TRUE(c.set_early_fold(true));
                c.tick(0.0);
            }
            for (int guard = 0; guard < 500; ++guard) {
                const TableSnapshot& s = c.snapshot();
                if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;
                if (s.phase == ControllerPhase::HeroToAct) { ASSERT_TRUE(c.submit_hero_action(passive(s.legal))); }
                run_bots(c);
            }
            ASSERT_NE(c.snapshot().phase, ControllerPhase::BotToAct);
            ASSERT_NE(c.snapshot().phase, ControllerPhase::HeroToAct);
            EXPECT_FALSE(c.snapshot().early_fold_armed);
            EXPECT_FALSE(c.snapshot().can_fold_early);
            EXPECT_FALSE(c.set_early_fold(true));
            if (c.snapshot().phase == ControllerPhase::SessionOver) c.new_session();
            else ASSERT_TRUE(c.next_hand());
            EXPECT_FALSE(c.snapshot().early_fold_armed);
            EXPECT_EQ(c.snapshot().seats[kHeroSeat].status, SeatStatus::Active);
        }
        return;
    }
    FAIL() << "no seed with a bot first to act";
}

// --- Revealing bot hands -----------------------------------------------------

TEST_F(Controller, RevealShowsHerosOwnFoldedCards) {
    LocalGameController c(options());
    int folded_hands = 0;
    for (int hand = 0; hand < 20; ++hand) {
        std::optional<std::array<Card, 2>> dealt;
        play_hand_out(c, [&](const TableSnapshot& s) {
            dealt = s.seats[kHeroSeat].visible_hole;
            return Action{s.legal.allows(ActionType::Check) ? ActionType::Check : ActionType::Fold};
        });
        const TableSnapshot before = c.snapshot();
        const SeatSnapshot& hero_before = before.seats[kHeroSeat];
        if (hero_before.status == SeatStatus::Folded && dealt) {
            ++folded_hands;
            EXPECT_FALSE(hero_before.holds_cards);
            EXPECT_TRUE(hero_before.revealed_after_fold);  // Hero's own cards never leave the table
            ASSERT_TRUE(hero_before.visible_hole);
            EXPECT_EQ(*hero_before.visible_hole, *dealt);

            ASSERT_TRUE(c.reveal_bot_hands());
            const SeatSnapshot& hero = c.snapshot().seats[kHeroSeat];
            EXPECT_TRUE(hero.revealed_after_fold);
            ASSERT_TRUE(hero.visible_hole);
            EXPECT_EQ(*hero.visible_hole, *dealt);
            EXPECT_EQ(hero.won, hero_before.won);
            EXPECT_EQ(hero.shown_hand, hero_before.shown_hand);
        }
        if (c.snapshot().phase == ControllerPhase::SessionOver) {
            c.new_session();
        } else {
            ASSERT_TRUE(c.next_hand());
        }
        EXPECT_FALSE(c.snapshot().seats[kHeroSeat].revealed_after_fold);
    }
    EXPECT_GT(folded_hands, 0);
}

TEST_F(Controller, HerosFoldedCardsStayVisibleMidHandWhileBotsStayHidden) {
    // Paced bots, one per tick, so the hand can be seen going on after Hero's fold.
    ControllerOptions o = options();
    o.instant_play = false;
    o.bot_delay_ms = 400.0;
    LocalGameController c(o);
    int checked = 0;
    for (int hand = 0; hand < 30 && checked < 5; ++hand) {
        std::optional<std::array<Card, 2>> dealt;
        bool folded = false;
        for (int guard = 0; guard < 500; ++guard) {
            const TableSnapshot& s = c.snapshot();
            if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;
            const SeatSnapshot& hero = s.seats[kHeroSeat];
            if (folded) {
                // The hand is still being played after Hero's fold.
                EXPECT_TRUE(hero.revealed_after_fold);
                ASSERT_TRUE(hero.visible_hole);
                EXPECT_EQ(*hero.visible_hole, *dealt);
                EXPECT_FALSE(s.bots_revealed);
                for (int seat = 1; seat < kMaxSeats; ++seat) {
                    EXPECT_FALSE(s.seats[seat].visible_hole.has_value()) << "seat " << seat;
                    EXPECT_FALSE(s.seats[seat].revealed_after_fold) << "seat " << seat;
                }
                ++checked;
            } else {
                EXPECT_FALSE(hero.revealed_after_fold);
            }
            if (s.phase == ControllerPhase::HeroToAct) {
                dealt = hero.visible_hole;
                if (s.legal.allows(ActionType::Fold)) {
                    ASSERT_TRUE(c.submit_hero_action({ActionType::Fold, 0}));
                    folded = true;
                } else {
                    ASSERT_TRUE(c.submit_hero_action(passive(s.legal)));
                }
            }
            c.tick(400.0);
        }
        if (c.snapshot().phase == ControllerPhase::SessionOver) {
            c.new_session();
        } else {
            ASSERT_TRUE(c.next_hand());
        }
    }
    EXPECT_GT(checked, 0);
}

TEST_F(Controller, RevealIsRefusedDuringAHandAndShowsEveryBotAfterIt) {
    fs::path log_path;
    std::vector<std::vector<std::string>> revealed_cards;
    {
        LocalGameController c(options());
        c.tick(0.0);
        for (int hand = 0; hand < 25; ++hand) {
            c.tick(0.0);
            if (c.snapshot().phase == ControllerPhase::HeroToAct) {
                EXPECT_FALSE(c.reveal_bot_hands());
                EXPECT_FALSE(c.snapshot().bots_revealed);
            }
            play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });

            const TableSnapshot before = c.snapshot();
            ASSERT_TRUE(c.reveal_bot_hands());
            const TableSnapshot& after = c.snapshot();
            EXPECT_TRUE(after.bots_revealed);
            expect_no_hidden_text(after);

            std::set<int> ids;
            for (const Card& card : after.board) ids.insert(card.id);
            std::vector<std::string> row(kMaxSeats);
            for (int seat = 0; seat < kMaxSeats; ++seat) {
                const SeatSnapshot& s = after.seats[seat];
                EXPECT_EQ(s.shown_hand, before.seats[seat].shown_hand);
                EXPECT_EQ(s.won, before.seats[seat].won);
                ASSERT_EQ(s.position.has_value(), s.visible_hole.has_value()) << "seat " << seat;
                EXPECT_EQ(s.revealed_after_fold, s.position && s.status == SeatStatus::Folded);
                if (s.visible_hole) {
                    EXPECT_TRUE(ids.insert((*s.visible_hole)[0].id).second);
                    EXPECT_TRUE(ids.insert((*s.visible_hole)[1].id).second);
                    row[seat] = (*s.visible_hole)[0].to_string() + (*s.visible_hole)[1].to_string();
                }
            }
            revealed_cards.push_back(std::move(row));

            if (after.phase == ControllerPhase::SessionOver) {
                c.new_session();
            } else {
                ASSERT_TRUE(c.next_hand());
            }
            EXPECT_FALSE(c.snapshot().bots_revealed);
            for (int seat = 1; seat < kMaxSeats; ++seat) {
                EXPECT_FALSE(c.snapshot().seats[seat].visible_hole.has_value());
                EXPECT_FALSE(c.snapshot().seats[seat].revealed_after_fold);
            }
        }
    }

    // Where the log records a seat's cards, they must be the ones revealed. The session
    // can restart, so hands are matched by order rather than by hand number.
    const auto lines = read_json_lines(hands());
    ASSERT_EQ(lines.size(), revealed_cards.size());
    int checked = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        for (const json& seat : lines[i]["seats"]) {
            if (!seat.contains("c")) continue;
            EXPECT_EQ(seat["c"].get<std::string>(), revealed_cards[i][seat["s"].get<int>()]);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
}

TEST_F(Controller, RevealingDoesNotChangeTheLogs) {
    const auto run = [&](const std::string& name, bool reveal) {
        ControllerOptions o = options();
        o.log_dir = dir_ / name;
        LocalGameController c(o);
        for (int hand = 0; hand < 15; ++hand) {
            play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });
            if (reveal) { ASSERT_TRUE(c.reveal_bot_hands()); }
            if (c.snapshot().phase == ControllerPhase::SessionOver) c.new_session();
            else ASSERT_TRUE(c.next_hand());
        }
    };
    run("plain", false);
    run("revealed", true);
    const auto plain = log_without_session_ids(dir_ / "plain" / "hand_history.jsonl");
    EXPECT_FALSE(plain.empty());
    EXPECT_EQ(plain, log_without_session_ids(dir_ / "revealed" / "hand_history.jsonl"));
}

// --- Live pots through the controller ----------------------------------------

TEST_F(Controller, PotBreakdownAlwaysAddsUpToTheTotal) {
    LocalGameController c(options(7));
    bool saw_side_pot = false;
    for (int session = 0; session < 6; ++session) {
        for (int hand = 0; hand < 80 && c.snapshot().phase != ControllerPhase::SessionOver; ++hand) {
            for (int guard = 0; guard < 500; ++guard) {
                const TableSnapshot& s = c.snapshot();
                int sum = s.pots.uncalled;
                for (const Pot& pot : s.pots.pots) sum += pot.amount;
                EXPECT_EQ(sum, s.pots.total);
                if (s.pots.pots.size() > 1) saw_side_pot = true;
                if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;
                if (s.phase == ControllerPhase::HeroToAct) {
                    // Shove often, so multi-way all-ins come up.
                    const Action a = s.legal.allows(ActionType::AllIn) ? Action{ActionType::AllIn} : passive(s.legal);
                    ASSERT_TRUE(c.submit_hero_action(a));
                }
                c.tick(0.0);
            }
            if (c.snapshot().phase == ControllerPhase::HandComplete) { ASSERT_TRUE(c.next_hand()); }
        }
        c.new_session();
    }
    EXPECT_TRUE(saw_side_pot);
}

TEST_F(Controller, FinishedHandPotIsWhatWasPaidOutNotReturnedChips) {
    LocalGameController c(options());
    bool saw_refund = false;
    for (int hand = 0; hand < 40; ++hand) {
        play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });
        const TableSnapshot& s = c.snapshot();
        int paid = 0;
        for (const PayoutLine& line : s.payouts) paid += line.amount;
        EXPECT_EQ(s.pots.total, paid);
        EXPECT_EQ(s.pots.uncalled, 0);
        if (s.refund_text) saw_refund = true;
        if (s.phase == ControllerPhase::SessionOver) c.new_session();
        else ASSERT_TRUE(c.next_hand());
    }
    EXPECT_TRUE(saw_refund);
}

// --- Logging and session lifecycle -------------------------------------------

TEST_F(Controller, EveryFinishedHandIsLoggedWithTheSessionIdAndShifterLabel) {
    std::string id;
    {
        LocalGameController c(options());
        id = c.snapshot().session.session_id;
        for (int hand = 0; hand < 25; ++hand) {
            play_hand_out(c, [](const TableSnapshot& s) { return Action{s.legal.allows(ActionType::Check)
                                                                            ? ActionType::Check
                                                                            : ActionType::Fold}; });
            if (c.snapshot().phase == ControllerPhase::SessionOver) break;
            ASSERT_TRUE(c.next_hand());
        }
    }

    const auto lines = read_json_lines(hands());
    ASSERT_GE(lines.size(), 24u);
    bool shifter_named = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i]["session_id"], id);
        EXPECT_EQ(lines[i]["h"], static_cast<int>(i) + 1);
        for (const json& seat : lines[i]["seats"]) {
            if (seat["s"] == 5) {
                shifter_named = true;
                EXPECT_EQ(seat["p"].get<std::string>().rfind("Shift:", 0), 0u);
            }
        }
    }
    EXPECT_TRUE(shifter_named);
}

TEST_F(Controller, SummaryWrittenOnceOnDestructionWithPublicNames) {
    std::string id;
    {
        LocalGameController c(options());
        id = c.snapshot().session.session_id;
    }
    const auto lines = read_json_lines(sessions());
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0]["session_id"], id);
    EXPECT_FALSE(lines[0]["done"]);
    EXPECT_EQ(lines[0]["seats"][5]["p"], "Shifter");
}

TEST_F(Controller, NewSessionWritesSummaryResetsAndGetsANewId) {
    LocalGameController c(options());
    const std::string first = c.snapshot().session.session_id;
    play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });

    c.new_session();
    const TableSnapshot& s = c.snapshot();
    EXPECT_NE(s.session.session_id, first);
    EXPECT_EQ(s.hand_number, 1);
    EXPECT_EQ(s.session.mistakes, 0);
    for (const SeatSnapshot& seat : s.seats) EXPECT_NE(seat.status, SeatStatus::Busted);

    const auto lines = read_json_lines(sessions());
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0]["session_id"], first);
}

// Pins the default lineup's logs across two sessions, so random personalities (off by
// default) can be shown to leave the classic table byte-identical, RNG streams included:
// a New Session must not rebuild the bots. The bots draw through library-defined
// distributions, so the constant is pinned for libstdc++ only.
TEST_F(Controller, OffModeLogsUnchanged) {
    {
        LocalGameController c(options(77));
        for (int session = 0; session < 2; ++session) {
            for (int hand = 0; hand < 25; ++hand) {
                play_hand_out(c, [](const TableSnapshot& s) {
                    // Calls once per street at most, so Hero sometimes folds and sometimes plays on.
                    if (s.legal.allows(ActionType::Check)) return Action{ActionType::Check, 0};
                    const bool cheap = s.legal.call_amount * 4 <= s.big_blind * 10;
                    return Action{cheap ? ActionType::Call : ActionType::Fold, 0};
                });
                if (c.snapshot().phase == ControllerPhase::SessionOver) break;
                ASSERT_TRUE(c.next_hand());
            }
            if (session == 0) c.new_session();
        }
    }
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    const auto add = [&](const std::string& text) {
        for (const char ch : text) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 0x100000001B3ULL;
        }
    };
    std::size_t lines = 0;
    for (const std::string& line : log_without_session_ids(hands())) {
        add(line);
        ++lines;
    }
    for (json line : read_json_lines(sessions())) {
        line.erase("session_id");
        line.erase("ts");
        add(line.dump());
    }
    EXPECT_GE(lines, 30u);
#if defined(__GLIBCXX__)
    EXPECT_EQ(hash, 0xb90533e223a1e356ULL) << std::hex << "hash 0x" << hash;
#else
    GTEST_SKIP() << std::hex << "no pinned value for this standard library; hash 0x" << hash;
#endif
}

TEST_F(Controller, MistakesAreRecordedFromThePreActionViewAndLogged) {
    {
        LocalGameController c(options(3));
        int mistakes = 0;
        for (int hand = 0; hand < 40 && mistakes == 0; ++hand) {
            play_hand_out(c, [](const TableSnapshot&) { return Action{ActionType::Fold}; });
            mistakes = c.snapshot().session.mistakes;
            if (c.snapshot().phase == ControllerPhase::SessionOver) break;
            if (mistakes == 0) { ASSERT_TRUE(c.next_hand()); }
        }
        ASSERT_GT(mistakes, 0) << "always folding never gave up EV";
        ASSERT_FALSE(c.snapshot().hand_mistakes.empty());
        EXPECT_FALSE(c.snapshot().hand_mistakes[0].recommended.empty());
        EXPECT_GT(c.snapshot().session.ev_lost, 0.0);
    }

    const auto lines = read_json_lines(hands());
    const json& mistake = lines.back()["hero_mistakes"][0];
    EXPECT_EQ(mistake["did"], "F");
    EXPECT_FALSE(mistake["why"].get<std::string>().empty());
    EXPECT_FALSE(mistake["rec"].get<std::string>().empty());

    const auto summary = read_json_lines(sessions());
    ASSERT_EQ(summary.size(), 1u);
    EXPECT_GT(summary[0]["hero"]["mistakes"].get<int>(), 0);
}

TEST_F(Controller, MistakeFromAnAbandonedHandDoesNotReachTheNextSession) {
    LocalGameController c(options(3));
    // Hero folds every hand until a fold is flagged with bots still left to act, so
    // the hand is unfinished and its mistake has not been written anywhere.
    bool noted = false;
    for (int hand = 0; hand < 200 && !noted; ++hand) {
        c.tick(0.0);
        if (c.snapshot().phase == ControllerPhase::HeroToAct) {
            ASSERT_TRUE(c.submit_hero_action({ActionType::Fold}));
            noted = !c.snapshot().hand_mistakes.empty() && c.snapshot().phase == ControllerPhase::BotToAct;
            c.set_instant_play(!noted);
        }
        if (noted) break;
        c.tick(0.0);
        ASSERT_TRUE(c.next_hand());
    }
    ASSERT_TRUE(noted) << "never reached an unfinished hand with a mistake";
    const std::string abandoned = c.snapshot().session.session_id;

    c.new_session();
    c.set_instant_play(true);
    EXPECT_TRUE(c.snapshot().hand_mistakes.empty());
    EXPECT_EQ(c.snapshot().session.mistakes, 0);

    // Passive play with Hero not folding cannot flag a free fold; the first hand's line
    // must carry none of the abandoned hand's mistakes unless it earned its own.
    play_hand_out(c, [](const TableSnapshot& s) { return s.hint->action; });
    const auto lines = read_json_lines(hands());
    EXPECT_NE(lines.back()["session_id"], abandoned);
    EXPECT_EQ(lines.back()["h"], 1);
    EXPECT_TRUE(lines.back()["hero_mistakes"].empty()) << lines.back().dump();
}

TEST_F(Controller, FreezeoutEndsWhenHeroBustsAndNextHandIsRefused) {
    LocalGameController c(options(11));
    for (int session = 0; session < 30; ++session) {
        for (int hand = 0; hand < 200 && c.snapshot().phase != ControllerPhase::SessionOver; ++hand) {
            play_hand_out(c, [](const TableSnapshot& s) {
                return Action{s.legal.allows(ActionType::AllIn) ? ActionType::AllIn : ActionType::Call};
            });
            if (c.snapshot().phase == ControllerPhase::HandComplete) { ASSERT_TRUE(c.next_hand()); }
        }
        const TableSnapshot& s = c.snapshot();
        if (s.phase == ControllerPhase::SessionOver && s.session.hero_busted) {
            EXPECT_FALSE(c.next_hand());
            EXPECT_EQ(s.session.hero_net, -2000);
            EXPECT_EQ(s.seats[kHeroSeat].status, SeatStatus::Busted);
            c.tick(1000.0);
            EXPECT_EQ(c.snapshot().phase, ControllerPhase::SessionOver);
            const auto summaries = read_json_lines(sessions());
            ASSERT_FALSE(summaries.empty());
            EXPECT_TRUE(summaries.back()["done"]);
            EXPECT_TRUE(summaries.back()["hero"]["bust"]);
            c.new_session();
            EXPECT_EQ(c.snapshot().seats[kHeroSeat].stack + c.snapshot().seats[kHeroSeat].street_bet, 2000);
            return;
        }
        c.new_session();
    }
    FAIL() << "Hero never busted while shoving every hand";
}

TEST_F(Controller, UnwritableLogDirectoryIsReportedButPlayContinues) {
    fs::create_directories(dir_);
    std::ofstream(dir_ / "blocked").put('x');
    ControllerOptions o = options();
    o.log_dir = dir_ / "blocked";  // A file, so nothing can be created inside it

    LocalGameController c(o);
    EXPECT_TRUE(c.snapshot().log_error.has_value());
    play_hand_out(c, [](const TableSnapshot& s) { return passive(s.legal); });
    EXPECT_NE(c.snapshot().phase, ControllerPhase::BotToAct);
}

// --- Solver mode -------------------------------------------------------------------

namespace {

bool contests(SeatStatus status) { return status == SeatStatus::Active || status == SeatStatus::AllIn; }

int contesting(const TableSnapshot& s) {
    int count = 0;
    for (const SeatSnapshot& seat : s.seats) count += contests(seat.status) ? 1 : 0;
    return count;
}

// Hero to act postflop heads-up against an opponent with chips behind: what the
// solver handles.
bool solvable(const TableSnapshot& s) {
    if (s.phase != ControllerPhase::HeroToAct || s.street == Street::Preflop || contesting(s) != 2) return false;
    for (int seat = 1; seat < kMaxSeats; ++seat) {
        if (s.seats[static_cast<std::size_t>(seat)].status == SeatStatus::AllIn) return false;
    }
    return true;
}

// Ticks until the solve for the current decision leaves Running, or the time is up.
const SolverProgress& wait_for_solver(IGameController& c, double timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (c.snapshot().solver.progress.state == SolverState::Running &&
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        c.tick(0.0);
    }
    return c.snapshot().solver.progress;
}

bool legal_for(const LegalActions& legal, const Action& action) {
    if (!legal.allows(action.type)) return false;
    if (action.type == ActionType::Bet || action.type == ActionType::Raise) {
        return action.amount >= legal.min_raise_to && action.amount <= legal.max_raise_to;
    }
    return true;
}

// Plays hands with Hero passive until `stop` returns true at a Hero decision; then
// returns with that decision pending. Fails after `hands` hands.
template <typename Stop>
bool play_until(LocalGameController& c, int hands, Stop stop) {
    for (int hand = 0; hand < hands; ++hand) {
        c.tick(0.0);
        for (int guard = 0; guard < 500; ++guard) {
            const TableSnapshot& s = c.snapshot();
            if (s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver) break;
            if (s.phase == ControllerPhase::HeroToAct) {
                if (stop(s)) return true;
                if (!c.submit_hero_action(passive(s.legal))) return false;
            }
            c.tick(1e9);
        }
        if (c.snapshot().phase == ControllerPhase::SessionOver) {
            c.new_session();
        } else if (!c.next_hand()) {
            return false;
        }
    }
    return false;
}

}  // namespace

TEST_F(Controller, SolverModeHeadsUpRiverProducesAdvice) {
    ControllerOptions o = options();
    o.solver_enabled = true;
    o.solver_tier_override = SolverTier::River;
    o.solver_fixed_iterations = 200;
    LocalGameController c(o);
    EXPECT_TRUE(c.solver_enabled());
    EXPECT_TRUE(c.snapshot().solver.available);
    EXPECT_EQ(c.snapshot().solver.tier, SolverTier::River);
    EXPECT_NE(c.snapshot().solver.hardware_summary.find("(tier set to River)"), std::string::npos);

    ASSERT_TRUE(play_until(c, 200, [](const TableSnapshot& s) { return solvable(s) && s.street == Street::River; }));
    const SolverProgress progress = wait_for_solver(c, 10'000);
    ASSERT_EQ(progress.state, SolverState::Done) << progress.note;
    EXPECT_EQ(progress.iterations, 200);
    ASSERT_TRUE(progress.advice);
    ASSERT_TRUE(progress.exploitability_pct);
    EXPECT_EQ(progress.advice->street_label, "River");
    const TableSnapshot& s = c.snapshot();
    double total = 0.0;
    for (const SolverOption& option : progress.advice->options) {
        EXPECT_TRUE(legal_for(s.legal, option.action)) << option.label;
        EXPECT_GE(option.freq, 0.0);
        total += option.freq;
    }
    EXPECT_NEAR(total, 1.0, 1e-3);
    EXPECT_TRUE(s.hint.has_value());  // The heuristic hint is still there
    ASSERT_TRUE(c.submit_hero_action(progress.advice->best));
    EXPECT_EQ(c.snapshot().solver.progress.state, SolverState::Off);
}

TEST_F(Controller, SolverModeMultiwayFallsBack) {
    ControllerOptions o = options();
    o.solver_enabled = true;
    o.solver_tier_override = SolverTier::Turn;
    LocalGameController c(o);
    ASSERT_TRUE(play_until(c, 200, [](const TableSnapshot& s) {
        return s.phase == ControllerPhase::HeroToAct && s.street != Street::Preflop && contesting(s) > 2;
    }));
    const SolverProgress progress = wait_for_solver(c, 10'000);
    EXPECT_EQ(progress.state, SolverState::NotApplicable);
    EXPECT_NE(progress.note.find("(multiway)"), std::string::npos) << progress.note;
    EXPECT_FALSE(progress.advice);
    EXPECT_TRUE(c.snapshot().hint.has_value());
}

// Solver mode only adds advice: the same seeded session logs the same hands and the
// same mistakes with it on, while solves run (and are cancelled) beside the play.
TEST_F(Controller, SolverDoesNotChangeMistakesOrLogs) {
    const auto run = [&](const std::string& name, bool solver) {
        ControllerOptions o = options(11);
        o.log_dir = dir_ / name;
        o.solver_enabled = solver;
        o.solver_tier_override = SolverTier::Turn;
        o.solver_budget_ms = 2000;
        o.solver_threads = 2;
        LocalGameController c(o);
        int solved = 0;
        for (int hand = 0; hand < 25; ++hand) {
            play_hand_out(c, [&](const TableSnapshot& s) {
                if (solver && solvable(s)) {
                    // Let the solve start, so it is running when Hero acts.
                    for (int i = 0; i < 5; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        c.tick(0.0);
                    }
                    ++solved;
                }
                return passive(s.legal);
            });
            if (c.snapshot().phase == ControllerPhase::SessionOver) c.new_session();
            else EXPECT_TRUE(c.next_hand());
        }
        return std::pair{c.snapshot().session, solved};
    };
    const auto [off, unused] = run("off", false);
    const auto [on, solved] = run("on", true);
    EXPECT_GT(solved, 0);
    EXPECT_EQ(off.mistakes, on.mistakes);
    EXPECT_EQ(off.ev_lost, on.ev_lost);
    EXPECT_EQ(off.hands_played, on.hands_played);
    const auto plain = log_without_session_ids(dir_ / "off" / "hand_history.jsonl");
    EXPECT_FALSE(plain.empty());
    EXPECT_EQ(plain, log_without_session_ids(dir_ / "on" / "hand_history.jsonl"));
}

TEST_F(Controller, SolverIsCancelledWhenHeroActs) {
    ControllerOptions o = options();
    o.instant_play = false;
    o.bot_delay_ms = 1e6;  // Bots act only when the test says so
    o.solver_tier_override = SolverTier::Turn;
    o.solver_budget_ms = 60'000;
    o.solver_threads = 2;
    LocalGameController c(o);
    EXPECT_FALSE(c.solver_enabled());
    EXPECT_EQ(c.solver_budget_ms(), 60'000);
    ASSERT_TRUE(play_until(c, 200, [](const TableSnapshot& s) { return solvable(s) && s.street != Street::River; }));
    EXPECT_EQ(c.snapshot().solver.progress.state, SolverState::Off);  // The mode is off

    // Turning it on solves the decision already pending.
    c.set_solver_enabled(true);
    EXPECT_EQ(c.snapshot().solver.progress.state, SolverState::Running);
    const auto start = std::chrono::steady_clock::now();
    while (c.snapshot().solver.progress.iterations == 0 && c.snapshot().solver.progress.state == SolverState::Running &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        c.tick(0.0);
    }
    ASSERT_EQ(c.snapshot().solver.progress.state, SolverState::Running) << c.snapshot().solver.progress.note;
    const std::string street_label = c.snapshot().solver.progress.note;
    EXPECT_TRUE(street_label == "Turn" || street_label == "Flop (approximate: no river betting)") << street_label;

    // Acting clears the panel at once, and nothing from the old solve comes back.
    ASSERT_TRUE(c.submit_hero_action(passive(c.snapshot().legal)));
    EXPECT_EQ(c.snapshot().solver.progress.state, SolverState::Off);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        c.tick(0.0);
        EXPECT_EQ(c.snapshot().solver.progress.state, SolverState::Off);
    }
    c.set_solver_budget_ms(3000);
    EXPECT_EQ(c.solver_budget_ms(), 3000);
    c.set_solver_enabled(false);
    EXPECT_FALSE(c.solver_enabled());
}
