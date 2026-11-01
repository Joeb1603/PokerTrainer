#include "app/trainer_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

namespace poker {
namespace {

constexpr ImU32 kFelt = IM_COL32(24, 92, 62, 255);
constexpr ImU32 kFeltEdge = IM_COL32(92, 62, 34, 255);
constexpr ImU32 kFeltLine = IM_COL32(40, 122, 86, 255);
constexpr ImU32 kPanel = IM_COL32(28, 32, 40, 240);
constexpr ImU32 kPanelFolded = IM_COL32(28, 32, 40, 130);
constexpr ImU32 kPanelBorder = IM_COL32(70, 78, 92, 255);
constexpr ImU32 kActing = IM_COL32(255, 201, 71, 255);
constexpr ImU32 kWinner = IM_COL32(92, 214, 132, 255);
constexpr ImU32 kText = IM_COL32(236, 238, 242, 255);
constexpr ImU32 kMuted = IM_COL32(160, 168, 180, 255);
constexpr ImU32 kAllIn = IM_COL32(240, 96, 88, 255);
constexpr ImU32 kCardFace = IM_COL32(250, 250, 246, 255);
constexpr ImU32 kCardEdge = IM_COL32(120, 124, 130, 255);
constexpr ImU32 kCardBack = IM_COL32(44, 70, 130, 255);
constexpr ImU32 kCardBackLine = IM_COL32(112, 140, 200, 255);
constexpr ImU32 kCardSlot = IM_COL32(16, 70, 46, 255);

constexpr ImVec4 kAccentText{1.0f, 0.79f, 0.28f, 1.0f};
constexpr ImVec4 kMutedText{0.63f, 0.66f, 0.71f, 1.0f};
constexpr ImVec4 kWarnText{0.94f, 0.45f, 0.40f, 1.0f};

ImU32 suit_color(Suit suit) {
    switch (suit) {
        case Suit::Spades: return IM_COL32(20, 20, 24, 255);
        case Suit::Hearts: return IM_COL32(208, 32, 44, 255);
        case Suit::Diamonds: return IM_COL32(28, 92, 214, 255);
        case Suit::Clubs: return IM_COL32(20, 140, 62, 255);
    }
    return IM_COL32_BLACK;
}

ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }

void text_at(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const std::string& text) {
    dl->AddText(font, size, pos, color, text.c_str());
}

ImVec2 text_size(ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, 10000.0f, 0.0f, text.c_str());
}

void text_centered(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 color, const std::string& text) {
    const ImVec2 extent = text_size(font, size, text);
    text_at(dl, font, size, {center.x - extent.x * 0.5f, center.y - extent.y * 0.5f}, color, text);
}

// Suits are drawn from shapes so they stay sharp at every size and need no font glyphs.
void draw_heart_shape(ImDrawList* dl, ImVec2 c, float r, ImU32 color, bool inverted) {
    const float dir = inverted ? -1.0f : 1.0f;
    const float lobe = r * 0.52f;
    dl->AddCircleFilled({c.x - lobe * 0.92f, c.y - dir * r * 0.32f}, lobe, color, 24);
    dl->AddCircleFilled({c.x + lobe * 0.92f, c.y - dir * r * 0.32f}, lobe, color, 24);
    dl->AddTriangleFilled({c.x - r * 0.96f, c.y - dir * r * 0.14f}, {c.x + r * 0.96f, c.y - dir * r * 0.14f},
                          {c.x, c.y + dir * r}, color);
}

void draw_stem(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddTriangleFilled({c.x, c.y + r * 0.1f}, {c.x - r * 0.42f, c.y + r}, {c.x + r * 0.42f, c.y + r}, color);
}

void draw_suit(ImDrawList* dl, Suit suit, ImVec2 c, float r) {
    const ImU32 color = suit_color(suit);
    switch (suit) {
        case Suit::Hearts:
            draw_heart_shape(dl, c, r, color, false);
            break;
        case Suit::Spades:
            draw_heart_shape(dl, {c.x, c.y - r * 0.08f}, r * 0.92f, color, true);
            draw_stem(dl, c, r, color);
            break;
        case Suit::Diamonds: {
            const ImVec2 points[4] = {{c.x, c.y - r}, {c.x + r * 0.74f, c.y}, {c.x, c.y + r}, {c.x - r * 0.74f, c.y}};
            dl->AddConvexPolyFilled(points, 4, color);
            break;
        }
        case Suit::Clubs: {
            const float lobe = r * 0.46f;
            dl->AddCircleFilled({c.x, c.y - r * 0.5f}, lobe, color, 24);
            dl->AddCircleFilled({c.x - r * 0.52f, c.y + r * 0.12f}, lobe, color, 24);
            dl->AddCircleFilled({c.x + r * 0.52f, c.y + r * 0.12f}, lobe, color, 24);
            draw_stem(dl, c, r, color);
            break;
        }
    }
}

std::string rank_text(Rank rank) {
    if (rank == Rank::Ten) return "10";
    return std::string(1, Card(rank, Suit::Clubs).to_string()[0]);
}

void draw_card_face(ImDrawList* dl, const UiFonts& fonts, Card card, ImVec2 pos, float w, float h) {
    const float rounding = w * 0.10f;
    const ImVec2 end{pos.x + w, pos.y + h};
    dl->AddRectFilled(pos, end, kCardFace, rounding);
    dl->AddRect(pos, end, kCardEdge, rounding, 0, 1.0f);

    const float rank_size = h * 0.40f;
    text_at(dl, fonts.bold, rank_size, {pos.x + w * 0.10f, pos.y + h * 0.03f}, suit_color(card.suit()),
            rank_text(card.rank()));
    draw_suit(dl, card.suit(), {pos.x + w * 0.60f, pos.y + h * 0.68f}, w * 0.28f);
}

void draw_card_back(ImDrawList* dl, ImVec2 pos, float w, float h) {
    const float rounding = w * 0.10f;
    const ImVec2 end{pos.x + w, pos.y + h};
    dl->AddRectFilled(pos, end, kCardBack, rounding);
    dl->AddRect(pos, end, kCardFace, rounding, 0, 1.5f);
    const float inset = w * 0.14f;
    dl->AddRect({pos.x + inset, pos.y + inset}, {end.x - inset, end.y - inset}, kCardBackLine, rounding * 0.5f);
    const ImVec2 c{pos.x + w * 0.5f, pos.y + h * 0.5f};
    const float r = w * 0.20f;
    const ImVec2 points[4] = {{c.x, c.y - r * 1.4f}, {c.x + r, c.y}, {c.x, c.y + r * 1.4f}, {c.x - r, c.y}};
    dl->AddConvexPolyFilled(points, 4, kCardBackLine);
}

void draw_pill(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 fill, ImU32 ink, const std::string& text) {
    const ImVec2 extent = text_size(font, size, text);
    const float pad_x = size * 0.55f;
    const float pad_y = size * 0.18f;
    const ImVec2 min{center.x - extent.x * 0.5f - pad_x, center.y - extent.y * 0.5f - pad_y};
    const ImVec2 max{center.x + extent.x * 0.5f + pad_x, center.y + extent.y * 0.5f + pad_y};
    dl->AddRectFilled(min, max, fill, (max.y - min.y) * 0.5f);
    text_centered(dl, font, size, center, ink, text);
}

ImU32 position_color(Position position) {
    switch (position) {
        case Position::BTN: return IM_COL32(236, 236, 236, 255);
        case Position::SB: return IM_COL32(118, 170, 240, 255);
        case Position::BB: return IM_COL32(240, 170, 90, 255);
        default: return IM_COL32(120, 130, 146, 255);
    }
}

std::string pot_eligibility_text(const Pot& pot, const TableSnapshot& s) {
    std::string text;
    for (std::size_t i = 0; i < pot.eligible.size(); ++i) {
        if (i > 0) text += ", ";
        text += s.seats[pot.eligible[i]].name;
    }
    return text;
}

bool sized_button(const char* label, bool enabled, float w, float h) {
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, {w, h});
    ImGui::EndDisabled();
    return clicked;
}

void push_button_color(float hue) {
    ImGui::PushStyleColor(ImGuiCol_Button, static_cast<ImVec4>(ImColor::HSV(hue, 0.55f, 0.55f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, static_cast<ImVec4>(ImColor::HSV(hue, 0.60f, 0.70f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, static_cast<ImVec4>(ImColor::HSV(hue, 0.65f, 0.82f)));
}

void draw_payout_lines(const TableSnapshot& s) {
    std::string text;
    for (const PayoutLine& line : s.payouts) {
        if (!text.empty()) text += "   |   ";
        text += line.name + " wins " + std::to_string(line.amount) + (line.hand ? " with " + *line.hand : std::string());
    }
    ImGui::TextColored(kAccentText, "%s", text.c_str());
    if (s.refund_text) {
        ImGui::SameLine();
        ImGui::TextColored(kMutedText, "%s", s.refund_text->c_str());
    }
}

constexpr ImGuiWindowFlags kFixedWindow = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

void begin_fixed(const char* name, float x, float y, float w, float h, ImGuiWindowFlags extra = 0) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin(name, nullptr, kFixedWindow | extra);
}

}  // namespace

TrainerUi::TrainerUi(IGameController& controller, UiFonts fonts, float scale)
    : controller_(controller), fonts_(fonts), scale_(scale) {}

void TrainerUi::draw() {
    handle_shortcuts();

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float sidebar_w = u(340.0f);
    const float console_h = u(136.0f);
    const float table_w = display.x - sidebar_w;

    draw_table(0.0f, 0.0f, table_w, display.y - console_h);
    draw_console(0.0f, display.y - console_h, table_w, console_h);
    draw_sidebar(table_w, 0.0f, sidebar_w, display.y);
}

void TrainerUi::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && controller_.snapshot().phase == ControllerPhase::HandComplete) {
        (void)controller_.next_hand();
    }
}

void TrainerUi::draw_table(float x, float y, float w, float h) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 19, 25, 255));
    begin_fixed("Table", x, y, w, h, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 center{x + w * 0.5f, y + h * 0.5f};
    const float table_w = w - u(150.0f);
    const float table_h = h - u(130.0f);
    const ImVec2 min{center.x - table_w * 0.5f, center.y - table_h * 0.5f};
    const ImVec2 max{center.x + table_w * 0.5f, center.y + table_h * 0.5f};
    const float rounding = table_h * 0.5f;

    dl->AddRectFilled({min.x - u(10), min.y - u(10)}, {max.x + u(10), max.y + u(10)}, kFeltEdge, rounding + u(10));
    dl->AddRectFilled(min, max, kFelt, rounding);
    dl->AddRect({min.x + u(14), min.y + u(14)}, {max.x - u(14), max.y - u(14)}, kFeltLine, rounding - u(14), 0, 1.5f);

    const TableSnapshot& s = controller_.snapshot();

    // Clockwise from Hero at bottom center, matching the engine's seat order.
    const float radius_x = table_w * 0.5f;
    const float radius_y = table_h * 0.5f;
    constexpr float kPi = 3.14159265f;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const float angle = kPi * 0.5f + static_cast<float>(seat) * kPi / 3.0f;
        const ImVec2 seat_center{center.x + std::cos(angle) * radius_x, center.y + std::sin(angle) * radius_y};
        draw_seat(dl, s.seats[seat], seat, seat_center, center);
    }

    draw_center(dl, center, table_h);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void TrainerUi::draw_seat(ImDrawList* dl, const SeatSnapshot& seat, int index, const ImVec2& target,
                          const ImVec2& table_center) {
    const bool is_hero = index == kHeroSeat;
    const float panel_w = u(is_hero ? 252.0f : 226.0f);
    const float panel_h = u(is_hero ? 104.0f : 84.0f);
    const float card_h = panel_h - u(16.0f);
    const float card_w = card_h * 0.70f;

    const ImVec2 canvas_min = ImGui::GetWindowPos();
    const ImVec2 canvas_size = ImGui::GetWindowSize();
    const ImVec2 min{std::clamp(target.x - panel_w * 0.5f, canvas_min.x + u(8), canvas_min.x + canvas_size.x - panel_w - u(8)),
                     std::clamp(target.y - panel_h * 0.5f, canvas_min.y + u(8), canvas_min.y + canvas_size.y - panel_h - u(8))};
    const ImVec2 max{min.x + panel_w, min.y + panel_h};
    const ImVec2 mid{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};

    const bool out = seat.status == SeatStatus::Folded || (seat.status == SeatStatus::Busted && !seat.holds_cards);
    const ImU32 border = seat.won > 0 ? kWinner : seat.is_to_act ? kActing : kPanelBorder;
    const float rounding = u(10.0f);
    dl->AddRectFilled(min, max, out ? kPanelFolded : kPanel, rounding);
    dl->AddRect(min, max, border, rounding, 0, (seat.is_to_act || seat.won > 0) ? u(2.5f) : 1.0f);

    // Cards.
    const float card_y = min.y + u(8.0f);
    const float cards_x = min.x + u(8.0f);
    if (seat.holds_cards || seat.revealed_after_fold) {
        for (std::size_t i = 0; i < 2; ++i) {
            const ImVec2 pos{cards_x + static_cast<float>(i) * (card_w + u(3.0f)), card_y};
            if (seat.visible_hole) {
                draw_card_face(dl, fonts_, (*seat.visible_hole)[i], pos, card_w, card_h);
                if (seat.revealed_after_fold) {
                    dl->AddRectFilled(pos, {pos.x + card_w, pos.y + card_h}, IM_COL32(16, 19, 25, 150), card_w * 0.10f);
                }
            } else {
                draw_card_back(dl, pos, card_w, card_h);
            }
        }
    }

    // Text column, to the right of the cards.
    const float text_x = cards_x + 2.0f * card_w + u(11.0f);
    const float name_size = u(is_hero ? 18.0f : 16.0f);
    const float small = u(14.0f);
    const ImU32 ink = out ? kMuted : kText;
    float line_y = min.y + u(7.0f);
    text_at(dl, fonts_.bold, name_size, {text_x, line_y}, ink, seat.name);

    if (seat.position) {
        const std::string label(to_string(*seat.position));
        const float pill_w = text_size(fonts_.bold, u(12.0f), label).x + u(14.0f);
        draw_pill(dl, fonts_.bold, u(12.0f), {max.x - pill_w * 0.5f - u(7.0f), line_y + name_size * 0.56f},
                  position_color(*seat.position), IM_COL32(16, 18, 24, 255), label);
    }

    line_y += name_size + u(4.0f);
    text_at(dl, fonts_.regular, small, {text_x, line_y}, kText, std::to_string(seat.stack));
    line_y += small + u(3.0f);

    std::string status;
    ImU32 status_color = kMuted;
    if (seat.won > 0) {
        status = "Won " + std::to_string(seat.won);
        status_color = kWinner;
    } else if (seat.status == SeatStatus::Busted) {
        status = "Busted";
        status_color = kAllIn;
    } else if (seat.status == SeatStatus::Folded) {
        status = "Folded";
    } else if (seat.status == SeatStatus::AllIn) {
        status = "All-In";
        status_color = kAllIn;
    } else if (seat.is_to_act) {
        status = "To act";
        status_color = kActing;
    }
    if (!status.empty()) text_at(dl, fonts_.bold, small, {text_x, line_y}, status_color, status);

    const TableSnapshot& table = controller_.snapshot();
    const std::optional<std::string>& hand_name = seat.shown_hand || !is_hero ? seat.shown_hand : table.hero_hand;
    if (hand_name) {
        const float hand_size = u(13.0f);
        draw_pill(dl, fonts_.regular, hand_size, {mid.x, max.y + hand_size * 0.2f}, IM_COL32(16, 18, 24, 245), kText,
                  *hand_name);
    }

    // Between the seat and the table center: the street bet and the dealer button.
    const ImVec2 to_center{table_center.x - mid.x, table_center.y - mid.y};
    const float length = std::max(1.0f, std::sqrt(to_center.x * to_center.x + to_center.y * to_center.y));
    const ImVec2 dir{to_center.x / length, to_center.y / length};
    const float reach_y = panel_h * 0.5f + u(20.0f);
    const float reach_x = panel_w * 0.5f + u(26.0f);
    const float reach = std::min(std::abs(dir.x) > 0.01f ? reach_x / std::abs(dir.x) : 1e6f,
                                 std::abs(dir.y) > 0.01f ? reach_y / std::abs(dir.y) : 1e6f);
    const ImVec2 bet_pos{mid.x + dir.x * reach, mid.y + dir.y * reach};

    if (seat.street_bet > 0) {
        draw_pill(dl, fonts_.bold, u(14.0f), bet_pos, IM_COL32(240, 226, 170, 255), IM_COL32(40, 32, 12, 255),
                  std::to_string(seat.street_bet));
    }
    if (seat.is_button) {
        const float r = u(11.0f);
        const float offset = seat.street_bet > 0 ? u(46.0f) : 0.0f;
        const ImVec2 c{bet_pos.x - dir.y * offset, bet_pos.y + dir.x * offset};
        dl->AddCircleFilled(c, r, IM_COL32(250, 250, 250, 255), 28);
        dl->AddCircle(c, r, IM_COL32(60, 60, 60, 255), 28, 1.0f);
        text_centered(dl, fonts_.bold, u(13.0f), c, IM_COL32(20, 20, 20, 255), "D");
    }
}

void TrainerUi::draw_center(ImDrawList* dl, const ImVec2& center, float table_h) {
    const TableSnapshot& s = controller_.snapshot();
    const float card_h = std::min(u(96.0f), table_h * 0.24f);
    const float card_w = card_h * 0.70f;
    const float gap = u(7.0f);
    const float row_w = 5.0f * card_w + 4.0f * gap;
    const float card_y = center.y - card_h * 0.5f;

    for (std::size_t i = 0; i < 5; ++i) {
        const ImVec2 pos{center.x - row_w * 0.5f + static_cast<float>(i) * (card_w + gap), card_y};
        if (i < s.board.size()) {
            draw_card_face(dl, fonts_, s.board[i], pos, card_w, card_h);
        } else {
            dl->AddRectFilled(pos, {pos.x + card_w, pos.y + card_h}, kCardSlot, card_w * 0.10f);
        }
    }

    // Total above the board, breakdown below it.
    text_centered(dl, fonts_.bold, u(20.0f), {center.x, card_y - u(18.0f)}, kText,
                  "Pot " + std::to_string(s.pots.total));

    float line_y = card_y + card_h + u(14.0f);
    const float line_h = u(17.0f);
    const bool complete = s.phase == ControllerPhase::HandComplete || s.phase == ControllerPhase::SessionOver;
    if (s.pots.pots.size() > 1) {
        for (std::size_t i = 0; i < s.pots.pots.size(); ++i) {
            const Pot& pot = s.pots.pots[i];
            std::string line = i == 0 ? "Main pot " : "Side pot " + std::to_string(i) + " ";
            line += std::to_string(pot.amount) + "  (" + pot_eligibility_text(pot, s) + ")";
            text_centered(dl, fonts_.regular, u(14.0f), {center.x, line_y}, IM_COL32(220, 232, 224, 255), line);
            line_y += line_h;
        }
        if (s.pots.uncalled > 0 && !complete) {
            text_centered(dl, fonts_.regular, u(14.0f), {center.x, line_y}, IM_COL32(200, 214, 206, 255),
                          "Uncalled " + std::to_string(s.pots.uncalled));
        }
    }
}

void TrainerUi::draw_console(float x, float y, float w, float h) {
    begin_fixed("Console", x, y, w, h);
    const TableSnapshot& s = controller_.snapshot();
    const float row_h = u(38.0f);

    const auto reveal_button = [&] {
        ImGui::SameLine();
        if (sized_button(s.bots_revealed ? "Bot Hands Revealed" : "Reveal Bot Hands", !s.bots_revealed, u(200.0f), row_h)) {
            (void)controller_.reveal_bot_hands();
        }
    };

    if (s.phase == ControllerPhase::SessionOver) {
        push_button_color(0.33f);
        if (ImGui::Button("Start New Game", {u(240.0f), row_h})) controller_.new_session();
        ImGui::PopStyleColor(3);
        reveal_button();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(fonts_.bold);
        ImGui::Text("%s", s.session.hero_busted ? "You busted" : "You won the freezeout");
        ImGui::PopFont();
        ImGui::Text("Hands %d   |   Net %+d chips   |   Flagged mistakes %d (about %.0f chips of estimated EV)",
                    s.session.hands_played, s.session.hero_net, s.session.mistakes, s.session.ev_lost);
        // The last hand stays on the table, so its result is still worth reading.
        draw_payout_lines(s);
    } else if (s.phase == ControllerPhase::HandComplete) {
        push_button_color(0.33f);
        if (ImGui::Button("Next Hand  (Space)", {u(240.0f), row_h})) (void)controller_.next_hand();
        ImGui::PopStyleColor(3);
        reveal_button();
        draw_payout_lines(s);
    } else if (s.phase == ControllerPhase::BotToAct) {
        if (s.early_fold_armed) {
            push_button_color(0.0f);
            if (ImGui::Button("Cancel Early Fold", {u(180.0f), row_h})) (void)controller_.set_early_fold(false);
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
        } else if (s.can_fold_early) {
            if (ImGui::Button("Fold Early", {u(180.0f), row_h})) (void)controller_.set_early_fold(true);
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        if (s.early_fold_armed) {
            ImGui::TextColored(kWarnText, "Folding when action reaches you...");
        } else if (s.seats[kHeroSeat].status == SeatStatus::Folded) {
            ImGui::TextColored(kMutedText, "You folded. Waiting for %s...", s.waiting_on->c_str());
        } else {
            ImGui::TextColored(kMutedText, "Waiting for %s...", s.waiting_on->c_str());
        }
    } else {
        const LegalActions& legal = s.legal;
        const bool can_raise = legal.allows(ActionType::Bet) || legal.allows(ActionType::Raise);
        const ActionType raise_type = legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
        const int all_in_to = s.seats[kHeroSeat].street_bet + s.seats[kHeroSeat].stack;

        // New decision: start the slider at the minimum.
        const std::pair<int, std::size_t> key{s.hand_number, s.action_log.size()};
        if (key != raise_key_) {
            raise_key_ = key;
            raise_to_ = legal.min_raise_to;
        }
        raise_to_ = std::clamp(raise_to_, legal.min_raise_to, std::max(legal.min_raise_to, legal.max_raise_to));

        std::optional<Action> chosen;

        push_button_color(0.0f);
        if (sized_button("Fold", legal.allows(ActionType::Fold), u(120.0f), row_h)) chosen = Action{ActionType::Fold};
        ImGui::PopStyleColor(3);
        ImGui::SameLine();

        push_button_color(0.58f);
        if (legal.allows(ActionType::Check)) {
            if (sized_button("Check", true, u(150.0f), row_h)) chosen = Action{ActionType::Check};
        } else {
            const std::string label = "Call " + std::to_string(legal.call_amount) +
                                      (legal.call_amount == s.seats[kHeroSeat].stack ? " (All-In)" : "");
            if (sized_button(label.c_str(), legal.allows(ActionType::Call), u(150.0f), row_h)) {
                chosen = Action{ActionType::Call};
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();

        push_button_color(0.33f);
        const std::string raise_label = can_raise ? (raise_type == ActionType::Bet ? "Bet " : "Raise to ") +
                                                        std::to_string(raise_to_) + " (" +
                                                        std::to_string(bet_pot_percent(legal, s.current_bet, s.pots.total, raise_to_)) +
                                                        "%)"
                                                  : "Raise";
        if (sized_button(raise_label.c_str(), can_raise, u(200.0f), row_h)) chosen = Action{raise_type, raise_to_};
        ImGui::PopStyleColor(3);
        ImGui::SameLine();

        push_button_color(0.08f);
        const std::string all_in_label = "All-In " + std::to_string(all_in_to);
        if (sized_button(all_in_label.c_str(), legal.allows(ActionType::AllIn), u(130.0f), row_h)) {
            chosen = Action{ActionType::AllIn};
        }
        ImGui::PopStyleColor(3);

        // Sizing.
        ImGui::BeginDisabled(!can_raise);
        ImGui::SetNextItemWidth(u(250.0f));
        if (can_raise && legal.min_raise_to < legal.max_raise_to) {
            ImGui::SliderInt("##raise", &raise_to_, legal.min_raise_to, legal.max_raise_to, "%d");
        } else {
            int fixed = raise_to_;
            ImGui::SliderInt("##raise", &fixed, fixed, fixed, can_raise ? "%d" : "-");
        }
        const auto quick = [&](const char* label, double fraction) {
            ImGui::SameLine();
            if (ImGui::Button(label, {u(68.0f), 0.0f})) {
                raise_to_ = quick_bet_to(legal, s.current_bet, s.pots.total, fraction);
            }
        };
        ImGui::SameLine();
        if (ImGui::Button("Min", {u(68.0f), 0.0f})) raise_to_ = legal.min_raise_to;
        quick("50% Pot", 0.5);
        quick("75% Pot", 0.75);
        quick("Pot", 1.0);
        ImGui::SameLine();
        if (ImGui::Button("All-In", {u(68.0f), 0.0f})) raise_to_ = legal.max_raise_to;
        ImGui::EndDisabled();

        if (s.hint) {
            const std::string gto_label = show_advisor_ ? "Play GTO: " + s.hint->best_action : "Play GTO";
            if (ImGui::Button(gto_label.c_str(), {u(420.0f), u(30.0f)})) chosen = s.hint->action;
        }

        if (chosen) (void)controller_.submit_hero_action(*chosen);
    }

    if (s.log_error) {
        if (s.phase == ControllerPhase::HeroToAct && s.hint) ImGui::SameLine();  // No spare row under the button
        ImGui::TextColored(kWarnText, "Logging is off: %s", s.log_error->c_str());
    }
    ImGui::End();
}

void TrainerUi::draw_sidebar(float x, float y, float w, float h) {
    begin_fixed("Sidebar", x, y, w, h);
    const TableSnapshot& s = controller_.snapshot();

    ImGui::PushFont(fonts_.bold);
    ImGui::Text("Hand %d", s.hand_number);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(kMutedText, "Blinds %d/%d", s.small_blind, s.big_blind);
    ImGui::TextColored(kMutedText, "Session %s", s.session.session_id.c_str());

    if (ImGui::Button("New Session / Reset Freezeout", {-1.0f, 0.0f})) controller_.new_session();

    bool instant = controller_.instant_play();
    if (ImGui::Checkbox("Instant bots", &instant)) controller_.set_instant_play(instant);
    ImGui::SameLine();
    ImGui::BeginDisabled(instant);
    float delay = static_cast<float>(controller_.bot_delay_ms());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##delay", &delay, 100.0f, 1000.0f, "%.0f ms")) {
        controller_.set_bot_delay_ms(delay);
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Training Advisor", &show_advisor_);
    ImGui::Separator();

    if (show_advisor_) draw_advisor();
    draw_action_log();
    ImGui::End();
}

void TrainerUi::draw_advisor() {
    const TableSnapshot& s = controller_.snapshot();
    if (s.hint) {
        const HintRecommendation& hint = *s.hint;
        ImGui::PushFont(fonts_.bold);
        ImGui::TextColored(kAccentText, "%s", hint.best_action.c_str());
        ImGui::PopFont();

        if (ImGui::BeginTable("##hint", 4, ImGuiTableFlags_SizingStretchSame)) {
            const auto stat = [](const char* name, double value, const char* format) {
                ImGui::TableNextColumn();
                ImGui::TextColored(kMutedText, "%s", name);
                ImGui::Text(format, value);
            };
            stat("Equity", hint.equity_pct, "%.1f%%");
            stat("Pot odds", hint.pot_odds_pct, "%.1f%%");
            stat("MDF", hint.mdf_pct, "%.1f%%");
            stat("SPR", hint.spr, "%.1f");
            ImGui::EndTable();
        }

        ImGui::TextWrapped("%s", hint.rationale.c_str());

        const auto ev_row = [](const char* name, const std::optional<double>& ev) {
            if (!ev) return;
            ImGui::TableNextColumn();
            ImGui::TextColored(kMutedText, "%s", name);
            ImGui::TableNextColumn();
            ImGui::Text("%+.0f", *ev);
        };
        if (ImGui::BeginTable("##ev", 4, ImGuiTableFlags_SizingStretchSame)) {
            ev_row("Fold", hint.ev.fold);
            ev_row("Chk/Call", hint.ev.check_call);
            ev_row("Raise 33%", hint.ev.raise_33);
            ev_row("Raise 75%", hint.ev.raise_75);
            ev_row("All-In", hint.ev.all_in);
            ImGui::EndTable();
        }
        ImGui::TextColored(kMutedText, "EVs are heuristic estimates, not solver output.");
    } else {
        ImGui::TextColored(kMutedText, "Hints appear when it is your turn.");
    }

    for (const HeroMistake& mistake : s.hand_mistakes) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnText);
        ImGui::TextWrapped("Flagged (%s): you played %s, advised %s", std::string(to_string(mistake.street)).c_str(),
                           mistake.taken.c_str(), mistake.recommended.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void TrainerUi::draw_action_log() {
    const TableSnapshot& s = controller_.snapshot();
    ImGui::TextColored(kMutedText, "This hand");
    ImGui::BeginChild("##log", {0.0f, 0.0f}, ImGuiChildFlags_Borders);
    for (const std::string& line : s.action_log) {
        if (line.rfind("--", 0) == 0) {
            ImGui::TextColored(kAccentText, "%s", line.c_str());
        } else {
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
    if (s.action_log.size() != log_lines_seen_) {
        log_lines_seen_ = s.action_log.size();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

}  // namespace poker
