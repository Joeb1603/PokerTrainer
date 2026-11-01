#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
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
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 1.0), 500);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.5), 300);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.75), 400);
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 0.0), 200);  // Min clamps up
    EXPECT_EQ(quick_bet_to(facing, 100, 300, 10.0), 1000);  // Stack clamps down

    LegalActions unbet;
    unbet.mask = 1u << static_cast<unsigned>(ActionType::Bet);
    unbet.min_raise_to = 20;
    unbet.max_raise_to = 1000;
    EXPECT_EQ(quick_bet_to(unbet, 0, 200, 0.5), 100);
    EXPECT_EQ(quick_bet_to(unbet, 0, 200, 1.0), 200);

    EXPECT_EQ(quick_bet_to(LegalActions{}, 0, 200, 1.0), 0);
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
            if (c.snapshot().phase == ControllerPhase::HandComplete) ASSERT_TRUE(c.next_hand());
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

TEST_F(Controller, MistakesAreRecordedFromThePreActionViewAndLogged) {
    {
        LocalGameController c(options(3));
        int mistakes = 0;
        for (int hand = 0; hand < 40 && mistakes == 0; ++hand) {
            play_hand_out(c, [](const TableSnapshot&) { return Action{ActionType::Fold}; });
            mistakes = c.snapshot().session.mistakes;
            if (c.snapshot().phase == ControllerPhase::SessionOver) break;
            if (mistakes == 0) ASSERT_TRUE(c.next_hand());
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
            if (c.snapshot().phase == ControllerPhase::HandComplete) ASSERT_TRUE(c.next_hand());
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
