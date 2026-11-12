#include "app/trainer_ui.h"

#include <algorithm>
#include <array>
#include <cfloat>
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
        text += s.seats[static_cast<std::size_t>(pot.eligible[i])].name;
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
        text += describe_payout(line);
    }
    float width = ImGui::CalcTextSize(text.c_str()).x;
    if (s.refund_text) width += ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(s.refund_text->c_str()).x;
    if (width <= ImGui::GetContentRegionAvail().x) {
        ImGui::TextColored(kAccentText, "%s", text.c_str());
        if (s.refund_text) {
            ImGui::SameLine();
            ImGui::TextColored(kMutedText, "%s", s.refund_text->c_str());
        }
        return;
    }
    // Split pots and kicker notes can run past one line.
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    if (s.refund_text) ImGui::TextColored(kMutedText, "%s", s.refund_text->c_str());
}

constexpr ImGuiWindowFlags kFixedWindow = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

void begin_fixed(const char* name, float x, float y, float w, float h, ImGuiWindowFlags extra = 0) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin(name, nullptr, kFixedWindow | extra);
}

// A tooltip for the last item that wraps, for sentences longer than a line.
void wrapped_tooltip(const char* text) {
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

constexpr const char* kRandomPersonalitiesHelp =
    "From the next session, the five bots get new names, new places at the table and playing styles drawn from a "
    "wider pool (tight, loose, aggressive, sticky, tricky and more), all different. The table shows only their "
    "names: work out how each one plays. Their styles are revealed when the session ends, and in the hand log.";

// Hero mistakes stay on the heuristic model's base_ev (CLAUDE.md section 11).
constexpr const char* kSolverMistakesNote =
    "Mistakes are judged against the heuristic Baseline, so following the solver can still be flagged.";

// A finished solve close enough to its target to follow (see solve_is_usable).
bool solve_converged(const SolverProgress& progress) {
    return progress.state == SolverState::Done && progress.advice && progress.exploitability_pct &&
           solve_is_usable(*progress.exploitability_pct, progress.advice->target_pct);
}

// The "Advice:" choices: generic player types, assumed for every opponent at once.
struct AdviceChoice {
    Archetype type;
    const char* label;
    const char* tip;
};

constexpr std::array<AdviceChoice, kArchetypeCount> kAdviceChoices{{
    {Archetype::Baseline, "Baseline", "Baseline: a solid regular. The default advice, and what mistakes are judged on."},
    {Archetype::Nit, "Nit",
     "Nit: plays few hands, folds too much and rarely bluffs. Steal and bluff more; give its bets and raises credit."},
    {Archetype::Station, "Station",
     "Calling station: calls too much, rarely raises or bluffs. Value bet thinner and bigger; don't bluff."},
    {Archetype::Maniac, "Maniac",
     "Maniac: bets, raises and bluffs too much. Call down lighter and let it bluff; bluff less yourself."},
}};

constexpr const char* kAdviceHelp =
    "Advice as if every opponent played like this type. A generic type, not a read on any seat.";

std::string label_of(Archetype type) { return std::string(to_string(type)); }

}  // namespace

std::string action_code_text(const std::string& code) {
    if (code == "F") return "FOLD";
    if (code == "X") return "CHECK";
    if (code.size() < 2 || !std::all_of(code.begin() + 1, code.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return code;
    }
    const std::string amount = code.substr(1);
    switch (code[0]) {
        case 'C': return "CALL " + amount;
        case 'B': return "BET " + amount;
        case 'R': return "RAISE to " + amount;
        case 'A': return "ALL-IN " + amount;
        default: return code;
    }
}

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
        draw_seat(dl, s.seats[static_cast<std::size_t>(seat)], seat, seat_center, center);
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
        std::string label = *hand_name;
        if (seat.shown_five) {
            // The five cards played, so a win on kickers doesn't read as a tie.
            label += ":";
            for (const Card& card : *seat.shown_five) label += std::string(" ") + card.to_string()[0];
        }
        const float hand_size = u(13.0f);
        draw_pill(dl, fonts_.regular, hand_size, {mid.x, max.y + hand_size * 0.2f}, IM_COL32(16, 18, 24, 245), kText,
                  label);
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
        if (!s.session.lineup.empty()) {
            // Random personalities: the styles are revealed once the freezeout is over.
            std::string lineup = "Lineup: ";
            for (std::size_t i = 0; i < s.session.lineup.size(); ++i) {
                if (i > 0) lineup += "   |   ";
                lineup += s.session.lineup[i];
            }
            ImGui::TextColored(kMutedText, "%s", lineup.c_str());
        }
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
                raise_to_ = quick_bet_to(legal, s.current_bet, s.pots.total, fraction, s.raise_cap_to);
            }
        };
        ImGui::SameLine();
        if (ImGui::Button("Min", {u(68.0f), 0.0f})) raise_to_ = legal.min_raise_to;
        quick("25%", 0.25);
        quick("50%", 0.5);
        quick("75%", 0.75);
        quick("Pot", 1.0);
        ImGui::SameLine();
        if (ImGui::Button("All-In", {u(68.0f), 0.0f})) raise_to_ = legal.max_raise_to;
        ImGui::EndDisabled();

        if (const SolverAdvice* solved = solver_advice()) {
            // Solver mode, Baseline selected and the solve for this decision done.
            const std::string gto_label = "Play GTO: " + solved->best_label + " (solver)";
            if (ImGui::Button(gto_label.c_str(), {u(420.0f), u(30.0f)})) chosen = solved->best;
            wrapped_tooltip(kSolverMistakesNote);
        } else if (s.hint) {
            // Follows the "Advice:" selector; Baseline when no line is selected or available.
            const AdviceLine* line = selected_line(*s.hint);
            const std::string name = line ? "Play vs " + label_of(line->versus) : "Play GTO";
            const std::string& best = line ? line->best_action : s.hint->best_action;
            const std::string gto_label = show_advisor_ ? name + ": " + best : name;
            if (ImGui::Button(gto_label.c_str(), {u(420.0f), u(30.0f)})) chosen = line ? line->action : s.hint->action;
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
    draw_solver_toggle();
    draw_advice_selector();
    draw_random_personalities_toggle();
    ImGui::Separator();

    if (show_advisor_) draw_advisor();
    draw_action_log();
    ImGui::End();
}

void TrainerUi::draw_advice_selector() {
    // Two rows of two, in columns: one row of four does not fit the sidebar.
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto radio_width = [&](const char* label) {
        return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Advice:");
    wrapped_tooltip(kAdviceHelp);
    ImGui::SameLine();
    const float first_column = ImGui::GetCursorPosX();
    const float second_column = first_column + std::max(radio_width(kAdviceChoices[0].label), radio_width(kAdviceChoices[2].label)) +
                                style.ItemSpacing.x;

    for (std::size_t i = 0; i < kAdviceChoices.size(); ++i) {
        const AdviceChoice& choice = kAdviceChoices[i];
        if (i % 2 == 1) {
            ImGui::SameLine(second_column);
        } else if (i > 0) {
            ImGui::SetCursorPosX(first_column);
        }
        if (ImGui::RadioButton(choice.label, advice_focus_ == choice.type)) advice_focus_ = choice.type;
        wrapped_tooltip(choice.tip);
    }
}

void TrainerUi::draw_random_personalities_toggle() {
    // Below the advice selector, so the rows above stay where they were.
    const TableSnapshot& s = controller_.snapshot();
    bool random = controller_.random_personalities();
    if (ImGui::Checkbox("Random personalities", &random)) controller_.set_random_personalities(random);
    wrapped_tooltip(kRandomPersonalitiesHelp);
    if (s.random_personalities != s.random_lineup_active) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMutedText, "(next session)");
        wrapped_tooltip("Takes effect when the next session starts: New Session, or Start New Game after this one.");
    }
}

const AdviceLine* TrainerUi::selected_line(const HintRecommendation& hint) const {
    if (advice_focus_ == Archetype::Baseline) return nullptr;
    for (const std::optional<AdviceLine>& line : hint.versus) {
        if (line && line->versus == advice_focus_) return &*line;
    }
    return nullptr;
}

void TrainerUi::draw_advisor() {
    if (!controller_.solver_enabled()) {
        draw_advisor_body(false);
        return;
    }
    // Solver mode adds a section, so the panel scrolls when it would leave the action
    // log less than a few lines; and while a solve applies, the heuristic reasoning
    // moves into the action's tooltip.
    const SolverState state = controller_.snapshot().solver.progress.state;
    const bool compact = state == SolverState::Running || state == SolverState::Done;
    const float max_h = std::max(u(120.0f), ImGui::GetContentRegionAvail().y - u(100.0f));
    ImGui::SetNextWindowSizeConstraints({0.0f, 0.0f}, {FLT_MAX, max_h});
    ImGui::BeginChild("##advisor", {0.0f, 0.0f}, ImGuiChildFlags_AutoResizeY);
    draw_advisor_body(compact);
    ImGui::EndChild();
}

void TrainerUi::draw_advisor_body(bool compact) {
    const TableSnapshot& s = controller_.snapshot();
    if (s.hint) {
        const HintRecommendation& hint = *s.hint;
        const AdviceLine* line = selected_line(hint);

        // The selected line's action, with Baseline's underneath when they differ.
        const std::string heading = line ? "Vs " + label_of(line->versus) + ": " + line->best_action : hint.best_action;
        ImGui::PushFont(fonts_.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
        ImGui::TextWrapped("%s", heading.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (compact) {
            const std::string tip = line ? line->why + "\n\nBaseline: " + hint.rationale : hint.rationale;
            wrapped_tooltip(tip.c_str());
        }
        if (line) {
            if (!compact) wrapped_tooltip(line->why.c_str());
            if (line->best_action != hint.best_action) {
                ImGui::TextColored(kMutedText, "Baseline: %s", hint.best_action.c_str());
            }
            // Also under the action, unless that would squeeze the action log too much.
            if (!compact && ImGui::GetContentRegionAvail().y >= u(560.0f)) ImGui::TextWrapped("%s", line->why.c_str());
        } else if (advice_focus_ != Archetype::Baseline) {
            ImGui::TextColored(kMutedText, "No %s line here; showing Baseline.", label_of(advice_focus_).c_str());
        }

        const double range_equity = line ? line->range_equity_pct : hint.range_equity_pct;
        if (ImGui::BeginTable("##hint", 4, ImGuiTableFlags_SizingStretchSame)) {
            const auto stat = [](const char* name, double value, const char* format) {
                ImGui::TableNextColumn();
                ImGui::BeginGroup();
                ImGui::TextColored(kMutedText, "%s", name);
                ImGui::Text(format, value);
                ImGui::EndGroup();
            };
            stat("Equity", range_equity, "%.1f%%");
            char equity_tip[96];
            std::snprintf(equity_tip, sizeof equity_tip, "vs estimated range %.1f%% / vs random hand %.1f%%", range_equity,
                          hint.equity_pct);
            wrapped_tooltip(equity_tip);
            stat("Pot odds", hint.pot_odds_pct, "%.1f%%");
            stat("MDF", hint.mdf_pct, "%.1f%%");
            stat("SPR", hint.spr, "%.1f");
            ImGui::EndTable();
        }

        // Baseline's reasoning; muted when another line leads.
        if (compact) {
            // In the heading's tooltip instead
        } else if (line) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
            ImGui::TextWrapped("Baseline: %s", hint.rationale.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextWrapped("%s", hint.rationale.c_str());
        }

        const EvEstimates& ev = line ? line->ev : hint.ev;
        const std::array<std::optional<BetOutcome>, kOutcomeCount>& outcomes = line ? line->outcomes : hint.outcomes;
        const auto ev_row = [](const char* name, const std::optional<double>& value,
                               const std::optional<BetOutcome>* outcome) {
            if (!value) return;
            char tip[96] = "";
            if (outcome && *outcome) {
                std::snprintf(tip, sizeof tip, "Folds %.0f%%, raises %.0f%%, you have %.0f%% when called",
                              (*outcome)->fold_pct, (*outcome)->raise_pct, (*outcome)->equity_called_pct);
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(kMutedText, "%s", name);
            if (tip[0] != '\0') wrapped_tooltip(tip);
            ImGui::TableNextColumn();
            const double rounded = std::round(*value);
            ImGui::Text("%+.0f", rounded == 0.0 ? 0.0 : rounded);  // No "-0"
            if (tip[0] != '\0') wrapped_tooltip(tip);
        };
        if (ImGui::BeginTable("##ev", 4, ImGuiTableFlags_SizingStretchSame)) {
            ev_row("Fold", ev.fold, nullptr);
            ev_row("Chk/Call", ev.check_call, nullptr);
            ev_row("Raise 25%", ev.raise_25, &outcomes[kOutcome25]);
            ev_row("Raise 50%", ev.raise_50, &outcomes[kOutcome50]);
            ev_row("Raise 75%", ev.raise_75, &outcomes[kOutcome75]);
            ev_row("Raise Pot", ev.raise_100, &outcomes[kOutcome100]);
            ev_row("All-In", ev.all_in, &outcomes[kOutcomeAllIn]);
            ImGui::EndTable();
        }

        // The three lines at a glance; clicking one selects it.
        const bool any_line = std::any_of(hint.versus.begin(), hint.versus.end(), [](const auto& l) { return l.has_value(); });
        if (any_line) {
            ImGui::TextColored(kMutedText, "Versus types");
            if (ImGui::BeginTable("##versus", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
                for (const std::optional<AdviceLine>& versus : hint.versus) {
                    if (!versus) continue;
                    const bool selected = advice_focus_ == versus->versus;
                    const std::string label = label_of(versus->versus) + "##versus";
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        advice_focus_ = versus->versus;
                    }
                    wrapped_tooltip(versus->why.c_str());
                    ImGui::TableNextColumn();
                    if (selected) {
                        ImGui::TextColored(kAccentText, "%s", versus->best_action.c_str());
                    } else {
                        ImGui::TextUnformatted(versus->best_action.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }

        if (controller_.solver_enabled()) draw_solver();

        ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
        ImGui::TextWrapped("Heuristic EVs vs assumed ranges, with a rule for bet size. Not solver output.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextColored(kMutedText, "Hints appear when it is your turn.");
        if (controller_.solver_enabled()) draw_solver();  // The budget can be set between decisions
    }

    for (const HeroMistake& mistake : s.hand_mistakes) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnText);
        ImGui::TextWrapped("Flagged (%s): you played %s, advised %s (judged vs Baseline)",
                           std::string(to_string(mistake.street)).c_str(), action_code_text(mistake.taken).c_str(),
                           mistake.recommended.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void TrainerUi::draw_solver_toggle() {
    // Beside the advisor toggle, so the rows below stay where they were. The solver's
    // output only shows, and Play GTO only follows it, while the advisor is shown.
    const SolverPanel& panel = controller_.snapshot().solver;
    ImGui::SameLine();
    ImGui::BeginDisabled(!panel.available);
    bool enabled = controller_.solver_enabled();
    if (ImGui::Checkbox("Solver mode", &enabled)) controller_.set_solver_enabled(enabled);
    ImGui::EndDisabled();
    const std::string tip =
        panel.available
            ? panel.hardware_summary + ".\n\nSolves your heads-up postflop decisions in the background with an approximate "
                                       "equilibrium solver (simplified bet sizes, assumed Baseline ranges), shown in the "
                                       "advisor panel. Preflop and multiway spots keep the heuristic advice."
            : "Unavailable on this machine: " + panel.hardware_summary + ".";
    wrapped_tooltip(tip.c_str());  // Shown while disabled too (ImGui's tooltip flags allow it)
}

const SolverAdvice* TrainerUi::solver_advice() const {
    const TableSnapshot& s = controller_.snapshot();
    const SolverProgress& progress = s.solver.progress;
    if (!show_advisor_ || advice_focus_ != Archetype::Baseline || !controller_.solver_enabled()) return nullptr;
    if (s.phase != ControllerPhase::HeroToAct || !solve_converged(progress)) return nullptr;
    return &*progress.advice;
}

void TrainerUi::draw_solver() {
    const TableSnapshot& s = controller_.snapshot();
    const SolverPanel& panel = s.solver;
    const SolverProgress& progress = panel.progress;
    ImGui::Separator();

    // Budget, in whole seconds. The flop tier goes to the full flop tree's minimum.
    const bool flop_tier = panel.tier == SolverTier::Flop;
    const int max_s = flop_tier ? kFlopFullMinBudgetMs / 1000 : 30;
    int budget_s = std::clamp((controller_.solver_budget_ms() + 500) / 1000, 2, max_s);
    const char* format = flop_tier && budget_s * 1000 >= kFlopFullMinBudgetMs ? "Budget %d s (full flop)" : "Budget %d s";
    ImGui::SetNextItemWidth(u(150.0f));
    if (ImGui::SliderInt("##solver_budget", &budget_s, 2, max_s, format, ImGuiSliderFlags_AlwaysClamp)) {
        controller_.set_solver_budget_ms(budget_s * 1000);
    }
    wrapped_tooltip(flop_tier ? "Solver time per decision, from the next decision on. A full flop solve needs at least "
                                "120 s; shorter budgets use the approximate flop tree (no river betting)."
                              : "Solver time per decision, from the next decision on.");

    switch (progress.state) {
        case SolverState::Off:
            return;
        case SolverState::NotApplicable:
        case SolverState::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, progress.state == SolverState::Failed ? kWarnText : kMutedText);
            ImGui::TextWrapped("%s", progress.note.c_str());
            ImGui::PopStyleColor();
            return;
        case SolverState::Running:
        case SolverState::Done:
            break;
    }

    const bool done = progress.state == SolverState::Done;
    ImGui::SameLine();
    ImGui::ProgressBar(done ? 1.0f : static_cast<float>(std::clamp(progress.progress, 0.0, 1.0)), {-1.0f, 0.0f},
                       done ? "Done" : nullptr);

    // "Turn: 340 iterations, exploitability 1.8% of pot"; "Starting..." while queued.
    const std::string street = progress.advice ? progress.advice->street_label : progress.note;
    char status[160];
    if (!done && progress.iterations == 0 && street == "Starting") {
        std::snprintf(status, sizeof status, "Starting\xE2\x80\xA6");  // U+2026, an ellipsis
    } else if (progress.exploitability_pct) {
        std::snprintf(status, sizeof status, "%s: %d iterations, exploitability %.1f%% of pot", street.c_str(),
                      progress.iterations, *progress.exploitability_pct);
    } else {
        std::snprintf(status, sizeof status, "%s: %d iterations", street.c_str(), progress.iterations);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
    ImGui::TextWrapped("%s", status);
    ImGui::PopStyleColor();
    wrapped_tooltip(
        "Exploitability: how much a perfect opponent could gain against this strategy, as a share of the pot. Lower is "
        "closer to equilibrium.");

    if (done && progress.advice) {
        const SolverAdvice& advice = *progress.advice;
        // Only a converged solve is followed: in the accent colour, and by Play GTO.
        const bool converged = solve_converged(progress);
        const ImVec4 highlight = converged ? kAccentText : kMutedText;
        ImGui::PushFont(fonts_.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, highlight);
        ImGui::TextWrapped("Solver (approximate): %s (most frequent)", advice.best_label.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        wrapped_tooltip(kSolverMistakesNote);
        if (!converged) {
            ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
            ImGui::TextWrapped("Not converged (%.1f%% of pot): Play GTO uses the heuristic advice. Raise the budget for a "
                               "closer solve.",
                               progress.exploitability_pct.value_or(100.0));
            ImGui::PopStyleColor();
        }

        // Action | freq | EV, in the tree's order (the panel scrolls if it is too tall).
        if (ImGui::BeginTable("##solver", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("freq", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("ev", ImGuiTableColumnFlags_WidthFixed);
            for (const SolverOption& option : advice.options) {
                const bool best = option.label == advice.best_label;
                const ImVec4 color = !converged ? kMutedText : best ? kAccentText : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", option.label.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%3.0f%%", option.freq * 100.0);
                ImGui::TableNextColumn();
                const double rounded = std::round(option.ev_chips);
                ImGui::TextColored(color, "%+.0f", rounded == 0.0 ? 0.0 : rounded);  // No "-0"
            }
            ImGui::EndTable();
        }
        wrapped_tooltip("How often the solver's strategy takes each action with your hand, and what each is worth in chips "
                        "from here.");
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
    ImGui::TextWrapped("Approximate solver: simplified bet sizes, assumed ranges.");
    ImGui::PopStyleColor();
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
