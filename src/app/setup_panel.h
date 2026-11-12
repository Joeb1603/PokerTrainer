#pragma once

#include <map>
#include <string>

#include "controller/blind_schedule.h"

struct ImFont;

namespace poker {

// The "New Game" screen: Freezeout or Tournament, the big blind, the starting stack
// and, for a tournament, the level length, a preset schedule that can be multiplied
// or edited level by level, and antes. It edits a draft GameSetup and reports Start
// or Cancel; the caller applies the result. It holds no game state.
class SetupPanel {
public:
    enum class Result { None, Start, Cancel };

    // Opens with `setup` as the draft. Cancel is offered only if `can_cancel`.
    void open(const GameSetup& setup, bool random_personalities, bool can_cancel);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // Draws the panel as a modal window centred on the display, over whatever else
    // is drawn. Call once a frame between ImGui::NewFrame and ImGui::Render while open.
    Result draw(ImFont* bold, float scale);

    // The draft as it stands; after Start, the setup to play (tournament levels left
    // empty when they are the preset's, so they follow it).
    const GameSetup& setup() const { return draft_; }
    bool random_personalities() const { return random_; }
    // Why Start is unavailable, or empty.
    const std::string& problem() const { return problem_; }

    // Where named controls were drawn last frame, in display pixels (for tests).
    struct Rect {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float cx() const { return (x0 + x1) * 0.5f; }
        float cy() const { return (y0 + y1) * 0.5f; }
    };
    const std::map<std::string, Rect>& rects() const { return rects_; }

private:
    void draw_common(float scale);
    void draw_tournament(float scale);
    void draw_levels(float scale);
    void rebuild_levels();  // From the preset, the big blind and the antes
    GameSetup normalized() const;  // The draft with levels cleared when they are the preset's
    void note(const std::string& name);

    bool open_ = false;
    bool popup_opened_ = false;
    bool can_cancel_ = true;
    bool random_ = false;
    GameSetup draft_;
    float factor_ = 1.5f;
    std::string problem_;
    std::map<std::string, Rect> rects_;
};

}  // namespace poker
