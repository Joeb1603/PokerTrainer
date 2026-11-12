// Runs the real TrainerUi in a windowless ImGui context, injecting input through
// ImGui's event queue, so buttons and shortcuts are tested without a display. The
// coordinates follow the layout at scale 1 in a 1360x860 window with the default style.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/trainer_ui.h"

using namespace poker;

namespace {

constexpr float kConsoleTop = 860.0f - 136.0f;
constexpr ImVec2 kFold{83.0f, kConsoleTop + 27.0f};
constexpr ImVec2 kCheckCall{241.0f, kConsoleTop + 27.0f};
constexpr ImVec2 kRaise{399.0f, kConsoleTop + 27.0f};
constexpr ImVec2 kAllIn{557.0f, kConsoleTop + 27.0f};
constexpr float kQuickRow = kConsoleTop + 60.0f;
constexpr ImVec2 kQuickMin{300.0f, kQuickRow};
constexpr ImVec2 kQuick25{376.0f, kQuickRow};
constexpr ImVec2 kQuick50{452.0f, kQuickRow};
constexpr ImVec2 kQuick75{528.0f, kQuickRow};
constexpr ImVec2 kQuickPot{604.0f, kQuickRow};
constexpr ImVec2 kQuickAllIn{680.0f, kQuickRow};
constexpr ImVec2 kNextHand{128.0f, kConsoleTop + 27.0f};
constexpr ImVec2 kNewSession{1190.0f, 52.0f};
constexpr ImVec2 kPlayGto{100.0f, kConsoleTop + 88.0f};
constexpr ImVec2 kFoldEarly{100.0f, kConsoleTop + 27.0f};
constexpr ImVec2 kReveal{356.0f, kConsoleTop + 27.0f};  // Right of Next Hand
constexpr ImVec2 kAdvisorToggle{1037.0f, 97.0f};
// Solver mode: beside the advisor toggle, on the same row.
constexpr ImVec2 kSolverToggle{1200.0f, 97.0f};
// The solver budget slider's two ends (150 px from the sidebar's left), with set_lines()'s
// hint and the solve running (its rationale moves into a tooltip).
constexpr ImVec2 kSolverBudgetMin{1030.0f, 341.0f};
constexpr ImVec2 kSolverBudgetMax{1176.0f, 341.0f};
// The "Advice:" selector under the advisor toggle: two rows of two radio buttons.
constexpr ImVec2 kAdviceBaseline{1095.0f, 120.0f};
constexpr ImVec2 kAdviceNit{1182.0f, 120.0f};
constexpr ImVec2 kAdviceStation{1095.0f, 143.0f};
constexpr ImVec2 kAdviceManiac{1182.0f, 143.0f};
// The "Versus types" Station row, with set_lines()'s hint (its one-line rationale).
constexpr ImVec2 kVersusStation{1040.0f, 314.0f};

std::uint8_t mask_of(std::initializer_list<ActionType> types) {
    std::uint8_t mask = 0;
    for (const ActionType type : types) mask |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(type));
    return mask;
}

class FakeController final : public IGameController {
public:
    TableSnapshot table;
    std::vector<Action> submitted;
    int next_hand_calls = 0;
    int new_session_calls = 0;
    int reveal_calls = 0;
    std::vector<bool> early_fold_requests;

    const TableSnapshot& snapshot() const override { return table; }
    bool set_early_fold(bool armed) override {
        early_fold_requests.push_back(armed);
        return true;
    }
    bool reveal_bot_hands() override {
        ++reveal_calls;
        return true;
    }
    void tick(double) override {}
    bool submit_hero_action(const Action& action) override {
        submitted.push_back(action);
        return true;
    }
    bool next_hand() override {
        ++next_hand_calls;
        return true;
    }
    void new_session() override { ++new_session_calls; }
    void set_bot_delay_ms(double) override {}
    double bot_delay_ms() const override { return 400.0; }
    void set_instant_play(bool) override {}
    bool instant_play() const override { return false; }
    void set_solver_enabled(bool enabled) override { solver_on = enabled; }
    bool solver_enabled() const override { return solver_on; }
    void set_solver_budget_ms(int budget_ms) override { solver_budget = budget_ms; }
    int solver_budget_ms() const override { return solver_budget; }
    void set_random_personalities(bool enabled) override {
        random_requests.push_back(enabled);
        table.random_personalities = enabled;
    }
    bool random_personalities() const override { return table.random_personalities; }

    bool solver_on = false;
    int solver_budget = 8000;
    std::vector<bool> random_requests;
};

class UiTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1360.0f, 860.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // No renderer needs the atlas
        ImFont* font = io.Fonts->AddFontDefault();

        fake_.table.small_blind = 10;
        fake_.table.big_blind = 20;
        for (int seat = 0; seat < kMaxSeats; ++seat) fake_.table.seats[static_cast<std::size_t>(seat)].name = "Seat" + std::to_string(seat);
        fake_.table.seats[kHeroSeat].stack = 1000;
        set_facing_raise();

        ui_.emplace(fake_, UiFonts{font, font}, 1.0f);
    }

    void TearDown() override { ImGui::DestroyContext(context_); }

    // Facing a 100 bet into a pot of 300: 100 to call, minimum raise to 200, 1000 behind.
    void set_facing_raise() {
        TableSnapshot& t = fake_.table;
        t.phase = ControllerPhase::HeroToAct;
        t.legal.mask = mask_of({ActionType::Fold, ActionType::Call, ActionType::Raise, ActionType::AllIn});
        t.legal.call_amount = 100;
        t.legal.min_raise_to = 200;
        t.legal.max_raise_to = 1000;
        t.current_bet = 100;
        t.raise_cap_to = 1000;
        t.pots.total = 300;
        t.action_log = {"a"};
    }

    void frame() {
        ImGui::NewFrame();
        ui_->draw();
        ImGui::Render();
    }

    void click(ImVec2 pos) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y);
        frame();
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
    }

    void press_space() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Space, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Space, false);
        frame();
    }

    // True if the console item under `pos` is the button with this exact label.
    bool console_button_is(ImVec2 pos, const char* label) {
        frame();  // The window must exist before it can be hovered
        ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
        frame();
        frame();
        ImGuiWindow* console = ImGui::FindWindowByName("Console");
        return console != nullptr && ImGui::GetCurrentContext()->HoveredIdPreviousFrame == console->GetID(label);
    }

    // True if the sidebar item under `pos` has this ID label.
    bool sidebar_item_is(ImVec2 pos, const char* label) {
        frame();
        ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
        frame();
        frame();
        ImGuiWindow* sidebar = ImGui::FindWindowByName("Sidebar");
        return sidebar != nullptr && ImGui::GetCurrentContext()->HoveredIdPreviousFrame == sidebar->GetID(label);
    }

    // True if the item under `pos` in the advisor panel (a child window of the sidebar
    // while solver mode is on) has this ID label.
    bool advisor_item_is(ImVec2 pos, const char* label) {
        for (int i = 0; i < 3; ++i) frame();  // The child sizes itself from the frames before
        ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
        frame();
        frame();
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if (std::string(window->Name).rfind("Sidebar/##advisor", 0) == 0 &&
                ImGui::GetCurrentContext()->HoveredIdPreviousFrame == window->GetID(label)) {
                return true;
            }
        }
        return false;
    }

    // Every piece of text one frame draws, clipped or not (ImGui's logging).
    std::string drawn_text() {
        for (int i = 0; i < 3; ++i) frame();
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        ui_->draw();
        ImGuiContext& g = *ImGui::GetCurrentContext();
        std::string text = g.LogBuffer.c_str();
        ImGui::LogFinish();
        g.LogBuffer.clear();
        ImGui::Render();
        return text;
    }

    // A Baseline hint plus the three archetype lines, each with a different action.
    void set_lines() {
        set_hint({ActionType::Raise, 500});
        HintRecommendation& hint = *fake_.table.hint;
        hint.rationale = "short";
        hint.ev.fold = 0.0;
        hint.ev.check_call = 1.0;
        hint.ev.raise_100 = 3.0;
        const auto line = [&](Archetype type, std::string best, Action action) {
            AdviceLine l;
            l.versus = type;
            l.best_action = std::move(best);
            l.action = action;
            l.ev = hint.ev;
            l.why = "why";
            return l;
        };
        hint.versus = {line(Archetype::Nit, "CALL", {ActionType::Call}),
                       line(Archetype::Station, "RAISE to 300 (50% Pot)", {ActionType::Raise, 300}),
                       line(Archetype::Maniac, "FOLD", {ActionType::Fold})};
    }

    // Solver mode on, available, and the solve for this decision in `state`. Done comes
    // with advice whose most frequent action is a call.
    void set_solver(SolverState state, SolverTier tier = SolverTier::Turn) {
        fake_.solver_on = true;
        SolverPanel& panel = fake_.table.solver;
        panel.available = true;
        panel.tier = tier;
        panel.hardware_summary = "4 threads, 15.6 GB free: solves river and turn, flop approximate";
        SolverProgress& progress = panel.progress;
        progress = {};
        progress.state = state;
        progress.progress = state == SolverState::Done ? 1.0 : 0.4;
        progress.iterations = 340;
        progress.exploitability_pct = 1.8;
        progress.note = state == SolverState::NotApplicable ? "Heads-up postflop only: showing heuristic advice (multiway)"
                        : state == SolverState::Failed      ? "Solver failed: out of memory"
                                                            : "Turn";
        if (state == SolverState::Done) {
            SolverAdvice advice;
            advice.options = {{"FOLD", {ActionType::Fold}, 0.1, 0.0},
                              {"CALL 100", {ActionType::Call}, 0.62, 41.6},
                              {"RAISE to 300 (50% Pot)", {ActionType::Raise, 300}, 0.2, 38.0},
                              {"RAISE to 500 (100% Pot)", {ActionType::Raise, 500}, 0.08, 30.2},
                              {"ALL-IN 1000", {ActionType::AllIn}, 0.0, -12.4}};
            advice.best = {ActionType::Call};
            advice.best_label = "CALL 100";
            advice.street_label = "Turn";
            advice.target_pct = 0.8;  // 1.8% is within 3x: converged
            progress.advice = advice;
        }
    }

    void set_hint(const Action& action) {
        HintRecommendation hint;
        hint.best_action = "RAISE to 500 (100% Pot)";
        hint.action = action;
        fake_.table.hint = hint;
    }

    ImGuiContext* context_ = nullptr;
    FakeController fake_;
    std::optional<TrainerUi> ui_;
};

void expect_single_action(const FakeController& fake, ActionType type, int amount = 0) {
    ASSERT_EQ(fake.submitted.size(), 1u);
    EXPECT_EQ(fake.submitted[0].type, type);
    EXPECT_EQ(fake.submitted[0].amount, amount);
}

}  // namespace

TEST_F(UiTest, FoldButtonSubmitsFold) {
    click(kFold);
    expect_single_action(fake_, ActionType::Fold);
}

TEST_F(UiTest, MiddleButtonCallsWhenFacingABet) {
    click(kCheckCall);
    expect_single_action(fake_, ActionType::Call);
}

TEST_F(UiTest, MiddleButtonChecksWhenNothingToCall) {
    fake_.table.legal.mask = mask_of({ActionType::Fold, ActionType::Check, ActionType::Bet, ActionType::AllIn});
    fake_.table.legal.call_amount = 0;
    click(kCheckCall);
    expect_single_action(fake_, ActionType::Check);
}

TEST_F(UiTest, RaiseStartsAtTheMinimum) {
    click(kRaise);
    expect_single_action(fake_, ActionType::Raise, 200);
}

TEST_F(UiTest, BetIsSentAsABetWhenNothingHasBeenBet) {
    fake_.table.legal.mask = mask_of({ActionType::Fold, ActionType::Check, ActionType::Bet, ActionType::AllIn});
    fake_.table.legal.call_amount = 0;
    fake_.table.legal.min_raise_to = 20;
    fake_.table.current_bet = 0;
    fake_.table.pots.total = 200;
    click(kQuickPot);
    click(kRaise);
    expect_single_action(fake_, ActionType::Bet, 200);
}

TEST_F(UiTest, AllInButtonSubmitsAllIn) {
    click(kAllIn);
    expect_single_action(fake_, ActionType::AllIn);
}

// Each quick button only sets the slider; the raise button is what acts. From a pot of
// 300 and 100 to call, the pot after calling is 400.
TEST_F(UiTest, QuickBetsSetTheSizeWithoutActing) {
    const auto size_after = [this](ImVec2 quick) {
        fake_.submitted.clear();
        click(quick);
        EXPECT_TRUE(fake_.submitted.empty()) << "a quick button acted by itself";
        click(kRaise);
        EXPECT_EQ(fake_.submitted.size(), 1u);
        const int amount = fake_.submitted.empty() ? -1 : fake_.submitted[0].amount;
        return amount;
    };
    EXPECT_EQ(size_after(kQuick25), 200);
    EXPECT_EQ(size_after(kQuick50), 300);
    EXPECT_EQ(size_after(kQuick75), 400);
    EXPECT_EQ(size_after(kQuickPot), 500);
    EXPECT_EQ(size_after(kQuickAllIn), 1000);
    EXPECT_EQ(size_after(kQuickMin), 200);
}

TEST_F(UiTest, QuickBetsStopAtWhatAnOpponentCanMatch) {
    fake_.table.raise_cap_to = 350;
    click(kQuickPot);
    click(kRaise);
    expect_single_action(fake_, ActionType::Raise, 350);
    fake_.submitted.clear();
    click(kQuickAllIn);  // All-in still means the whole stack
    click(kRaise);
    expect_single_action(fake_, ActionType::Raise, 1000);
}

TEST_F(UiTest, SliderResetsToTheMinimumForEachNewDecision) {
    frame();
    click(kQuickPot);
    fake_.table.action_log.push_back("next decision");
    frame();
    click(kRaise);
    expect_single_action(fake_, ActionType::Raise, 200);
}

TEST_F(UiTest, IllegalButtonsAreDisabled) {
    fake_.table.legal.mask = mask_of({ActionType::Fold, ActionType::Call});
    fake_.table.legal.min_raise_to = 0;
    fake_.table.legal.max_raise_to = 0;
    click(kRaise);
    click(kAllIn);
    click(kQuickPot);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, NoActionButtonsWhileABotIsToAct) {
    fake_.table.phase = ControllerPhase::BotToAct;
    fake_.table.legal = {};
    fake_.table.waiting_on = "Nit";
    click(kFold);
    click(kRaise);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, NextHandButtonAndSpacebarDealOnlyWhenTheHandIsComplete) {
    press_space();
    EXPECT_EQ(fake_.next_hand_calls, 0);

    fake_.table.phase = ControllerPhase::BotToAct;
    fake_.table.legal = {};
    fake_.table.waiting_on = "Nit";
    press_space();
    EXPECT_EQ(fake_.next_hand_calls, 0);

    fake_.table.phase = ControllerPhase::HandComplete;
    PayoutLine hero_win;
    hero_win.name = "Hero";
    hero_win.amount = 300;
    hero_win.hand = "Pair of Aces";
    fake_.table.payouts = {hero_win};
    press_space();
    EXPECT_EQ(fake_.next_hand_calls, 1);

    click(kNextHand);
    EXPECT_EQ(fake_.next_hand_calls, 2);
}

TEST_F(UiTest, HoldingSpaceDealsOnlyOnce) {
    fake_.table.phase = ControllerPhase::HandComplete;
    fake_.table.legal = {};
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Space, true);
    for (int i = 0; i < 120; ++i) frame();
    io.AddKeyEvent(ImGuiKey_Space, false);
    frame();
    EXPECT_EQ(fake_.next_hand_calls, 1);
}

TEST_F(UiTest, SessionOverOffersANewGameAndIgnoresSpace) {
    fake_.table.phase = ControllerPhase::SessionOver;
    fake_.table.legal = {};
    fake_.table.session.hero_busted = true;
    press_space();
    EXPECT_EQ(fake_.next_hand_calls, 0);

    click(kNextHand);  // The same spot holds Start New Game here
    EXPECT_EQ(fake_.new_session_calls, 1);
}

TEST_F(UiTest, ResetButtonStartsANewSessionMidHand) {
    click(kNewSession);
    EXPECT_EQ(fake_.new_session_calls, 1);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, RaiseButtonShowsThePotPercentageAsTheSizeChanges) {
    EXPECT_TRUE(console_button_is(kRaise, "Raise to 200 (25%)"));
    click(kQuick50);
    EXPECT_TRUE(console_button_is(kRaise, "Raise to 300 (50%)"));
    click(kQuickPot);
    EXPECT_TRUE(console_button_is(kRaise, "Raise to 500 (100%)"));
    click(kQuickAllIn);
    EXPECT_TRUE(console_button_is(kRaise, "Raise to 1000 (225%)"));
}

TEST_F(UiTest, BetButtonShowsThePotPercentage) {
    fake_.table.legal.mask = mask_of({ActionType::Fold, ActionType::Check, ActionType::Bet, ActionType::AllIn});
    fake_.table.legal.call_amount = 0;
    fake_.table.legal.min_raise_to = 20;
    fake_.table.current_bet = 0;
    fake_.table.pots.total = 200;
    click(kQuick75);
    EXPECT_TRUE(console_button_is(kRaise, "Bet 150 (75%)"));
}

TEST_F(UiTest, PlayGtoSubmitsExactlyTheHintsAction) {
    set_hint({ActionType::Raise, 500});
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
}

TEST_F(UiTest, PlayGtoAppearsOnlyWithAHint) {
    fake_.table.hint.reset();
    click(kPlayGto);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, PlayGtoLabelHidesTheAdviceWhenTheAdvisorIsOff) {
    set_hint({ActionType::Raise, 500});
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
    click(kAdvisorToggle);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
    click(kAdvisorToggle);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
}

TEST_F(UiTest, FoldEarlyArmsAndCancels) {
    fake_.table.phase = ControllerPhase::BotToAct;
    fake_.table.legal = {};
    fake_.table.waiting_on = "Nit";
    fake_.table.can_fold_early = true;
    click(kFoldEarly);
    ASSERT_EQ(fake_.early_fold_requests.size(), 1u);
    EXPECT_TRUE(fake_.early_fold_requests[0]);

    fake_.table.early_fold_armed = true;
    click(kFoldEarly);
    ASSERT_EQ(fake_.early_fold_requests.size(), 2u);
    EXPECT_FALSE(fake_.early_fold_requests[1]);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, FoldEarlyIsAbsentWhenHeroCannotUseIt) {
    fake_.table.phase = ControllerPhase::BotToAct;
    fake_.table.legal = {};
    fake_.table.waiting_on = "Nit";
    fake_.table.can_fold_early = false;
    click(kFoldEarly);
    EXPECT_TRUE(fake_.early_fold_requests.empty());
}

TEST_F(UiTest, RevealButtonWorksOnceAtTheEndOfAHand) {
    click(kReveal);  // During Hero's turn this spot is not the reveal button
    EXPECT_EQ(fake_.reveal_calls, 0);

    fake_.table.phase = ControllerPhase::HandComplete;
    fake_.table.legal = {};
    click(kReveal);
    EXPECT_EQ(fake_.reveal_calls, 1);

    fake_.table.bots_revealed = true;
    click(kReveal);
    EXPECT_EQ(fake_.reveal_calls, 1);

    fake_.table.phase = ControllerPhase::SessionOver;
    fake_.table.bots_revealed = false;
    click(kReveal);
    EXPECT_EQ(fake_.reveal_calls, 2);
}

TEST_F(UiTest, EveryScreenDrawsWithFullData) {
    TableSnapshot& t = fake_.table;
    t.hero_hand = "Two Pair, Aces and Kings";
    t.seats[1].revealed_after_fold = true;
    t.seats[1].visible_hole = std::array<Card, 2>{Card(Rank::Three, Suit::Clubs), Card(Rank::Four, Suit::Clubs)};
    t.board = {Card(Rank::Ace, Suit::Spades), Card(Rank::King, Suit::Hearts), Card(Rank::Ten, Suit::Diamonds),
               Card(Rank::Two, Suit::Clubs), Card(Rank::Nine, Suit::Spades)};
    t.pots.pots = {Pot{300, {0, 1, 2}}, Pot{400, {1, 2}}};
    t.pots.uncalled = 50;
    t.log_error = "disk full";
    t.hand_mistakes.push_back({Street::Flop, "C40", "FOLD", 20.0, 30.0, 60.0, 25.0, "why"});

    HintRecommendation hint;
    hint.best_action = "RAISE to 150 (75% Pot)";
    hint.rationale = "A rationale long enough that it has to wrap across the width of the sidebar panel.";
    hint.ev.fold = 0.0;
    hint.ev.check_call = 12.0;
    hint.ev.raise_25 = 20.0;
    hint.ev.raise_50 = 25.0;
    hint.ev.raise_75 = 30.0;
    hint.ev.raise_100 = 35.0;
    hint.ev.all_in = -40.0;
    hint.equity_pct = 65.6;
    hint.range_equity_pct = 46.8;
    const BetOutcome outcome{45.0, 6.0, 38.0, 20.0};
    hint.outcomes = {outcome, outcome, outcome, outcome, outcome};
    for (std::size_t i = 0; i < hint.versus.size(); ++i) {
        AdviceLine line;
        line.versus = static_cast<Archetype>(i + 1);
        line.best_action = i == 0 ? hint.best_action : "CHECK";
        line.action = {ActionType::Check};
        line.ev = hint.ev;
        line.range_equity_pct = 40.0 + static_cast<double>(i);
        line.outcomes = hint.outcomes;
        line.why.clear();
        while (line.why.size() < 140) line.why += "word ";  // The longest a why can be
        hint.versus[i] = line;
    }
    t.hint = hint;

    const Card ace(Rank::Ace, Suit::Hearts);
    const Card two(Rank::Two, Suit::Diamonds);
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        SeatSnapshot& s = t.seats[static_cast<std::size_t>(seat)];
        s.position = static_cast<Position>(seat);
        s.holds_cards = seat != 1;
        s.street_bet = seat * 10;
        s.status = static_cast<SeatStatus>(seat % 4);
        s.is_button = seat == 2;
        s.is_to_act = seat == 0;
        s.won = seat == 3 ? 100 : 0;
    }
    t.seats[kHeroSeat].visible_hole = std::array<Card, 2>{ace, two};
    t.seats[2].shown_hand = "Pair of Aces";

    const auto every_phase = [&] {
        for (const ControllerPhase phase : {ControllerPhase::HeroToAct, ControllerPhase::BotToAct,
                                            ControllerPhase::HandComplete, ControllerPhase::SessionOver}) {
            t.phase = phase;
            t.waiting_on = "Nit";
            for (int focus = 0; focus < kArchetypeCount; ++focus) {
                ui_->set_advice_focus(static_cast<Archetype>(focus));
                for (int i = 0; i < 3; ++i) frame();
            }
        }
    };
    every_phase();

    // Solver mode in every state, on each tier, with a status line long enough to wrap.
    for (const SolverState state : {SolverState::Off, SolverState::NotApplicable, SolverState::Running,
                                    SolverState::Done, SolverState::Failed}) {
        for (const SolverTier tier : {SolverTier::River, SolverTier::Turn, SolverTier::Flop}) {
            set_solver(state, tier);
            if (t.solver.progress.advice) t.solver.progress.advice->street_label = "Flop (approximate: no river betting)";
            t.hint = hint;
            every_phase();
        }
    }
    t.solver.progress.exploitability_pct.reset();
    every_phase();
    t.solver = {};  // Unavailable, with the mode still on
    every_phase();
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
}


TEST_F(UiTest, PlayGtoFollowsTheSelectedLine) {
    set_lines();
    click(kAdviceStation);
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 300);

    fake_.submitted.clear();
    click(kAdviceNit);
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Call);

    fake_.submitted.clear();
    click(kAdviceManiac);
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Fold);

    fake_.submitted.clear();
    click(kAdviceBaseline);
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
}

TEST_F(UiTest, PlayVsLabelNamesTheArchetype) {
    set_lines();
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
    click(kAdviceNit);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play vs Nit: CALL"));
    click(kAdviceStation);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play vs Station: RAISE to 300 (50% Pot)"));

    click(kAdvisorToggle);  // Hidden advice: the label names no action, the line still plays
    EXPECT_TRUE(console_button_is(kPlayGto, "Play vs Station"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 300);
}

TEST_F(UiTest, PlayGtoFallsBackToBaselineWithoutLines) {
    set_hint({ActionType::Raise, 500});  // No archetype lines
    click(kAdviceManiac);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
}

TEST_F(UiTest, VersusRowSelectsItsLine) {
    set_lines();
    click(kVersusStation);
    EXPECT_TRUE(fake_.submitted.empty());
    EXPECT_EQ(ui_->advice_focus(), Archetype::Station);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play vs Station: RAISE to 300 (50% Pot)"));
}

TEST_F(UiTest, ExistingSidebarButtonsDidNotMove) {
    set_lines();
    EXPECT_TRUE(sidebar_item_is(kNewSession, "New Session / Reset Freezeout"));
    EXPECT_TRUE(sidebar_item_is(kAdvisorToggle, "Training Advisor"));
    EXPECT_TRUE(sidebar_item_is(kAdviceBaseline, "Baseline"));
    EXPECT_TRUE(sidebar_item_is(kAdviceNit, "Nit"));
    EXPECT_TRUE(sidebar_item_is(kAdviceStation, "Station"));
    EXPECT_TRUE(sidebar_item_is(kAdviceManiac, "Maniac"));

    // Solver mode adds its checkbox beside the toggle and a section under the panel.
    for (const SolverState state : {SolverState::Running, SolverState::Done}) {
        set_solver(state);
        EXPECT_TRUE(sidebar_item_is(kSolverToggle, "Solver mode"));
        EXPECT_TRUE(sidebar_item_is(kNewSession, "New Session / Reset Freezeout"));
        EXPECT_TRUE(sidebar_item_is(kAdvisorToggle, "Training Advisor"));
        EXPECT_TRUE(sidebar_item_is(kAdviceBaseline, "Baseline"));
        EXPECT_TRUE(sidebar_item_is(kAdviceManiac, "Maniac"));
    }
    fake_.solver_on = false;
    fake_.table.solver = {};

    click(kNewSession);
    EXPECT_EQ(fake_.new_session_calls, 1);
    click(kAdvisorToggle);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO"));
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, SolverToggleCallsController) {
    fake_.table.solver.available = true;
    EXPECT_TRUE(sidebar_item_is(kSolverToggle, "Solver mode"));
    click(kSolverToggle);
    EXPECT_TRUE(fake_.solver_on);
    click(kSolverToggle);
    EXPECT_FALSE(fake_.solver_on);
    EXPECT_TRUE(fake_.submitted.empty());
}

TEST_F(UiTest, SolverToggleDisabledWhenUnavailable) {
    fake_.table.solver.available = false;
    fake_.table.solver.hardware_summary = "1 thread, 0.5 GB free: solver unavailable (needs 2 threads and 1 GB free)";
    click(kSolverToggle);
    EXPECT_FALSE(fake_.solver_on);

    // Its tooltip still explains why.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(kSolverToggle.x, kSolverToggle.y);
    for (int i = 0; i < 30; ++i) frame();
    ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
    EXPECT_TRUE(tooltip != nullptr && tooltip->LastFrameActive == ImGui::GetFrameCount());
}

// The budget slider's ends, in a hint with a one-line rationale: 2 to 30 s, or to the
// full flop tree's 120 s on the flop tier.
TEST_F(UiTest, SolverBudgetSliderSetsTheBudget) {
    set_lines();
    set_solver(SolverState::Running);
    EXPECT_TRUE(advisor_item_is(kSolverBudgetMax, "##solver_budget"));
    EXPECT_TRUE(advisor_item_is(kSolverBudgetMin, "##solver_budget"));
    click(kSolverBudgetMax);
    EXPECT_EQ(fake_.solver_budget, 30000);
    click(kSolverBudgetMin);
    EXPECT_EQ(fake_.solver_budget, 2000);

    set_solver(SolverState::Running, SolverTier::Flop);
    click(kSolverBudgetMax);
    EXPECT_EQ(fake_.solver_budget, 120000);
}

TEST_F(UiTest, PlayGtoUsesSolverWhenDone) {
    set_hint({ActionType::Raise, 500});
    set_solver(SolverState::Done);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: CALL 100 (solver)"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Call);
}

// Done is not enough: past 3x the street's target (turn 0.8%, so 2.4%) the solve is
// shown muted with a note, and Play GTO keeps the heuristic line.
TEST_F(UiTest, PlayGtoIgnoresAnUnconvergedSolve) {
    set_hint({ActionType::Raise, 500});
    set_solver(SolverState::Done);
    fake_.table.solver.progress.exploitability_pct = 10.0;
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
    const std::string text = drawn_text();
    EXPECT_NE(text.find("Solver (approximate): CALL 100 (most frequent)"), std::string::npos) << text;
    EXPECT_NE(text.find("Not converged (10.0% of pot): Play GTO uses the heuristic advice."), std::string::npos) << text;
    EXPECT_EQ(text.find("(solver)"), std::string::npos) << text;

    // Without a final exploitability, or right at the limit.
    fake_.table.solver.progress.exploitability_pct.reset();
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
    fake_.table.solver.progress.exploitability_pct = 2.4;
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: CALL 100 (solver)"));
}

// The heading says the solver is approximate, the mistakes note is in its tooltip,
// and a queued job reads "Starting...".
TEST_F(UiTest, SolverHeadingSaysApproximate) {
    set_lines();
    set_solver(SolverState::Done);
    std::string text = drawn_text();
    EXPECT_NE(text.find("Solver (approximate): CALL 100 (most frequent)"), std::string::npos) << text;
    EXPECT_NE(text.find("Approximate solver: simplified bet sizes, assumed ranges."), std::string::npos) << text;
    EXPECT_EQ(text.find("Not converged"), std::string::npos) << text;

    set_solver(SolverState::Running);
    fake_.table.solver.progress.iterations = 0;
    fake_.table.solver.progress.exploitability_pct.reset();
    fake_.table.solver.progress.note = "Starting";
    text = drawn_text();
    EXPECT_NE(text.find("Starting\xE2\x80\xA6"), std::string::npos) << text;
    EXPECT_EQ(text.find("0 iterations"), std::string::npos) << text;
}

TEST(SolverUsable, ThreeTimesTheTarget) {
    EXPECT_TRUE(solve_is_usable(0.9, target_exploitability_pct(SolverTree::River)));
    EXPECT_FALSE(solve_is_usable(0.91, target_exploitability_pct(SolverTree::River)));
    EXPECT_TRUE(solve_is_usable(2.4, target_exploitability_pct(SolverTree::Turn)));
    EXPECT_FALSE(solve_is_usable(10.0, target_exploitability_pct(SolverTree::Turn)));
    EXPECT_TRUE(solve_is_usable(5.8, target_exploitability_pct(SolverTree::FlopCheckDown)));
    EXPECT_FALSE(solve_is_usable(6.1, target_exploitability_pct(SolverTree::FlopFull)));
    EXPECT_FALSE(solve_is_usable(0.0, 0.0));  // No target: never usable
}

TEST_F(UiTest, PlayGtoUsesHeuristicWhileRunning) {
    set_hint({ActionType::Raise, 500});
    for (const SolverState state : {SolverState::Running, SolverState::NotApplicable, SolverState::Failed}) {
        set_solver(state);
        fake_.submitted.clear();
        EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
        click(kPlayGto);
        expect_single_action(fake_, ActionType::Raise, 500);
    }

    // A finished solve counts only while solver mode is on.
    set_solver(SolverState::Done);
    fake_.solver_on = false;
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: RAISE to 500 (100% Pot)"));
}

// The solver assumes Baseline ranges, so an archetype line keeps its own action; and
// with the advisor hidden, the solver has no effect.
TEST_F(UiTest, PlayGtoUsesHeuristicForArchetypeLines) {
    set_lines();
    set_solver(SolverState::Done);
    click(kAdviceStation);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play vs Station: RAISE to 300 (50% Pot)"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 300);

    fake_.submitted.clear();
    click(kAdviceBaseline);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO: CALL 100 (solver)"));
    click(kAdvisorToggle);
    EXPECT_TRUE(console_button_is(kPlayGto, "Play GTO"));
    click(kPlayGto);
    expect_single_action(fake_, ActionType::Raise, 500);
}

TEST_F(UiTest, TooltipAppearsOnHover) {
    set_lines();
    const auto tooltip_shown = [] {
        ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
        return tooltip != nullptr && tooltip->LastFrameActive == ImGui::GetFrameCount();
    };
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(700.0f, 400.0f);  // Over the table: no tooltip
    for (int i = 0; i < 30; ++i) frame();
    EXPECT_FALSE(tooltip_shown());

    io.AddMousePosEvent(kAdviceStation.x, kAdviceStation.y);
    for (int i = 0; i < 30; ++i) frame();
    EXPECT_TRUE(tooltip_shown());
}

TEST(TrainerUiText, ActionCodesReadAsAdvice) {
    EXPECT_EQ(action_code_text("F"), "FOLD");
    EXPECT_EQ(action_code_text("X"), "CHECK");
    EXPECT_EQ(action_code_text("C40"), "CALL 40");
    EXPECT_EQ(action_code_text("B60"), "BET 60");
    EXPECT_EQ(action_code_text("R120"), "RAISE to 120");
    EXPECT_EQ(action_code_text("A300"), "ALL-IN 300");
    EXPECT_EQ(action_code_text("Q5"), "Q5");
    EXPECT_EQ(action_code_text("C"), "C");
    EXPECT_EQ(action_code_text("R1x"), "R1x");
}
