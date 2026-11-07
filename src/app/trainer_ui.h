#pragma once

#include <cstddef>
#include <utility>

#include "controller/game_controller.h"

struct ImFont;
struct ImDrawList;
struct ImVec2;

namespace poker {

struct UiFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

// Draws the table and handles input. It sees the game only through IGameController,
// and holds no game state of its own apart from the raise slider.
class TrainerUi {
public:
    TrainerUi(IGameController& controller, UiFonts fonts, float scale);

    // Call once a frame, between ImGui::NewFrame and ImGui::Render.
    void draw();

    // Which line the advisor panel and Play GTO follow (the "Advice:" selector).
    // Generic player types applied to every opponent, never a seat's identity.
    void set_advice_focus(Archetype focus) { advice_focus_ = focus; }
    Archetype advice_focus() const { return advice_focus_; }

private:
    void draw_table(float x, float y, float w, float h);
    void draw_seat(ImDrawList* dl, const SeatSnapshot& seat, int index, const ImVec2& center, const ImVec2& table_center);
    void draw_center(ImDrawList* dl, const ImVec2& center, float table_h);
    void draw_console(float x, float y, float w, float h);
    void draw_sidebar(float x, float y, float w, float h);
    void draw_advice_selector();
    void draw_advisor();
    // compact: the reasoning goes into the heading's tooltip (solver mode, to make room).
    void draw_advisor_body(bool compact);
    // The selected archetype line, or nullptr for Baseline (also when the hint has
    // no lines, so the advice falls back to Baseline).
    const AdviceLine* selected_line(const HintRecommendation& hint) const;
    // Solver mode: the checkbox beside the advisor toggle, and the section at the
    // bottom of the advisor panel.
    void draw_solver_toggle();
    void draw_solver();
    // The finished solve for Hero's current decision, when Play GTO should follow it:
    // solver mode on, the advisor shown and Baseline selected (the solver assumes
    // Baseline ranges). Otherwise nullptr, and the heuristic line is played.
    const SolverAdvice* solver_advice() const;
    void draw_action_log();
    void handle_shortcuts();

    float u(float value) const { return value * scale_; }

    IGameController& controller_;
    UiFonts fonts_;
    float scale_;

    // Off keeps the advice hidden, including from the Play GTO button.
    bool show_advisor_ = true;
    Archetype advice_focus_ = Archetype::Baseline;  // UI-only state
    int raise_to_ = 0;
    // Identifies the current decision, so the slider resets once for each one.
    std::pair<int, std::size_t> raise_key_{-1, 0};
    std::size_t log_lines_seen_ = 0;
};

}  // namespace poker
