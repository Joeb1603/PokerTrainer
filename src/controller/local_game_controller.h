#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "advisor/training_advisor.h"
#include "bots/bot_strategy.h"
#include "controller/game_controller.h"
#include "engine/game_engine.h"
#include "logging/hand_logger.h"

namespace poker {

struct ControllerOptions {
    std::uint64_t seed = 0;
    std::filesystem::path log_dir = "logs";
    double bot_delay_ms = 400.0;
    bool instant_play = false;
    int starting_stack = 2000;
    int small_blind = 10;
    int big_blind = 20;
    int bot_rollouts = kDefaultRollouts;
    int advisor_rollouts = kDefaultRollouts;
};

// Runs a local freezeout: Hero in seat 0 against Nit, Loose, GTO, WildGTO and Shifter
// in seats 1 to 5. Bots act on the caller's thread, which is fine at about 2 ms a
// decision. Each hand is logged as it ends; a session summary is written when the
// freezeout ends, on new_session() and on destruction.
class LocalGameController final : public IGameController {
public:
    explicit LocalGameController(const ControllerOptions& options);
    ~LocalGameController() override;

    LocalGameController(const LocalGameController&) = delete;
    LocalGameController& operator=(const LocalGameController&) = delete;

    const TableSnapshot& snapshot() const override { return snapshot_; }
    void tick(double elapsed_ms) override;
    [[nodiscard]] bool submit_hero_action(const Action& action) override;
    [[nodiscard]] bool next_hand() override;
    [[nodiscard]] bool set_early_fold(bool armed) override;
    [[nodiscard]] bool reveal_bot_hands() override;
    void new_session() override;

    void set_bot_delay_ms(double delay_ms) override { bot_delay_ms_ = delay_ms; }
    double bot_delay_ms() const override { return bot_delay_ms_; }
    void set_instant_play(bool instant) override { instant_play_ = instant; }
    bool instant_play() const override { return instant_play_; }

private:
    void open_session();
    void begin_hand();
    void play_one_bot_action();
    void after_state_change();
    void on_hand_finished();
    void write_session_summary() noexcept;
    HandLogContext log_context(bool per_hand_labels) const;
    void rebuild_snapshot();
    std::vector<std::string> build_action_log() const;
    void note_log_failure(const std::exception& error) noexcept;

    ControllerOptions options_;
    GameEngine engine_;
    TrainingAdvisor advisor_;
    std::array<std::unique_ptr<IBotStrategy>, kMaxSeats> bots_;  // Seat 0 is Hero
    std::array<std::string, kMaxSeats> public_names_;
    std::array<int, kMaxSeats> starting_stacks_{};

    std::optional<HandLogger> hand_logger_;
    std::optional<SessionLogger> session_logger_;
    std::string session_id_;
    bool session_written_ = false;
    bool hand_handled_ = true;
    int session_mistakes_ = 0;
    double session_ev_lost_ = 0.0;
    std::vector<HeroMistake> hand_mistakes_;

    // The view and hint for Hero's pending decision, kept for record_hero_decision.
    std::optional<PlayerView> hero_view_;
    std::optional<HintRecommendation> hero_hint_;

    double bot_delay_ms_;
    bool instant_play_;
    double bot_timer_ms_ = 0.0;
    bool early_fold_armed_ = false;
    bool bots_revealed_ = false;
    std::optional<std::string> log_error_;
    TableSnapshot snapshot_;
};

}  // namespace poker
