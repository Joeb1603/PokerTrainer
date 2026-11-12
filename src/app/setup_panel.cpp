#include "app/setup_panel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <imgui.h>

namespace poker {
namespace {

constexpr const char* kPopupName = "New Game";
constexpr ImVec4 kMutedText{0.63f, 0.66f, 0.71f, 1.0f};
constexpr ImVec4 kWarnText{0.94f, 0.45f, 0.40f, 1.0f};

constexpr std::array<const char*, 2> kUnitNames{"hands", "minutes"};
constexpr std::array<const char*, 3> kAnteNames{"None", "Per player", "Big blind ante"};

constexpr const char* kModeHelp =
    "Freezeout: fixed blinds until someone has every chip (or you bust). Tournament: the same six seats and no "
    "rebuys, but the blinds rise level by level.";
constexpr const char* kBigBlindHelp =
    "The big blind in chips; the small blind is half of it. In a tournament this is level 1, and changing it "
    "rebuilds the levels from the schedule.";
constexpr const char* kAnteHelp =
    "Antes are dead money in the pot. Per player: everyone posts one before the blinds. Big blind ante: the big "
    "blind posts one for the whole table. Changing the type or the first level sets every level's ante again.";
constexpr const char* kMultiplyHelp =
    "Multiplies every level's blinds and ante, rounded to tidy amounts. Below 1 makes the levels smaller.";

void tooltip(const char* text) {
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

int ante_index(AnteType type) { return static_cast<int>(type); }

void label(const char* text, float column) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(column);
}

}  // namespace

void SetupPanel::open(const GameSetup& setup, bool random_personalities, bool can_cancel) {
    draft_ = setup;
    if (draft_.mode == GameMode::Tournament && draft_.levels.empty()) draft_.levels = draft_.schedule();
    random_ = random_personalities;
    can_cancel_ = can_cancel;
    open_ = true;
    popup_opened_ = false;
    problem_.clear();
}

void SetupPanel::note(const char* name) {
    if (!record_rects_) return;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    rects_[name] = Rect{min.x, min.y, max.x, max.y};
}

void SetupPanel::note_level(std::size_t index, const char* what) {
    if (!record_rects_) return;
    note(("Level " + std::to_string(index + 1) + " " + what).c_str());
}

GameSetup SetupPanel::normalized() const {
    GameSetup out = draft_;
    if (out.mode == GameMode::Freezeout ||
        out.levels == preset_schedule(out.preset, out.big_blind, out.ante_type, out.ante_from_level)) {
        out.levels.clear();  // The preset as is: kept by name
    }
    return out;
}

void SetupPanel::rebuild_levels() {
    draft_.levels = preset_schedule(draft_.preset, draft_.big_blind, draft_.ante_type, draft_.ante_from_level);
}

SetupPanel::Result SetupPanel::draw(ImFont* bold, float scale) {
    if (!open_) return Result::None;
    const auto u = [scale](float value) { return value * scale; };
    if (!popup_opened_) {
        ImGui::OpenPopup(kPopupName);
        popup_opened_ = true;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool tournament = draft_.mode == GameMode::Tournament;
    const ImVec2 size{u(600.0f), u(tournament ? 650.0f : 250.0f)};
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    Result result = Result::None;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::BeginPopupModal(kPopupName, nullptr, kFlags)) {
        popup_opened_ = false;  // Closed by ImGui itself; open it again next frame
        return Result::None;
    }

    if (bold) ImGui::PushFont(bold);
    ImGui::TextUnformatted("New Game");
    if (bold) ImGui::PopFont();
    ImGui::Separator();

    draw_common(scale);
    if (draft_.mode == GameMode::Tournament) draw_tournament(scale);

    const std::optional<std::string> problem = validate(draft_);
    problem_ = problem.value_or("");
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                         ImGui::GetFrameHeightWithSpacing() - ImGui::GetTextLineHeightWithSpacing() - u(4.0f));
    if (problem) {
        ImGui::TextColored(kWarnText, "%s", problem->c_str());
    } else {
        ImGui::TextColored(kMutedText, "%s", describe(normalized()).c_str());
    }

    ImGui::BeginDisabled(problem.has_value());
    ImGui::PushStyleColor(ImGuiCol_Button, static_cast<ImVec4>(ImColor::HSV(0.33f, 0.55f, 0.55f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, static_cast<ImVec4>(ImColor::HSV(0.33f, 0.60f, 0.70f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, static_cast<ImVec4>(ImColor::HSV(0.33f, 0.65f, 0.82f)));
    const bool start = ImGui::Button("Start", {u(160.0f), 0.0f});
    note("Start");
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (start && !problem) {
        draft_ = normalized();
        result = Result::Start;
    }
    if (can_cancel_) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {u(120.0f), 0.0f}) ||
            (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput)) {
            result = Result::Cancel;
        }
        note("Cancel");
    }

    if (result != Result::None) {
        ImGui::CloseCurrentPopup();
        open_ = false;
        popup_opened_ = false;
    }
    ImGui::EndPopup();
    return result;
}

void SetupPanel::draw_common(float scale) {
    const auto u = [scale](float value) { return value * scale; };
    const float column = u(150.0f);

    label("Game", column);
    if (ImGui::RadioButton("Freezeout", draft_.mode == GameMode::Freezeout)) draft_.mode = GameMode::Freezeout;
    note("Freezeout");
    tooltip(kModeHelp);
    ImGui::SameLine();
    if (ImGui::RadioButton("Tournament", draft_.mode == GameMode::Tournament)) {
        draft_.mode = GameMode::Tournament;
        // Level 1 is the big blind above, which may have changed under Freezeout.
        if (draft_.levels.empty() || draft_.levels.front().big_blind != draft_.big_blind) rebuild_levels();
    }
    note("Tournament");
    tooltip(kModeHelp);

    label("Big blind", column);
    ImGui::SetNextItemWidth(u(120.0f));
    int big_blind = draft_.big_blind;
    if (ImGui::InputInt("##big_blind", &big_blind, 0, 0)) {
        draft_.big_blind = std::clamp(big_blind, 0, kMaxBigBlind * 10);
        if (draft_.mode == GameMode::Tournament) rebuild_levels();
    }
    note("Big blind");
    tooltip(kBigBlindHelp);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMutedText, "chips (small blind %d)", small_blind_for(std::max(1, draft_.big_blind)));

    label("Starting stack", column);
    ImGui::SetNextItemWidth(u(120.0f));
    int starting_bb = draft_.starting_bb;
    if (ImGui::InputInt("##starting_bb", &starting_bb, 0, 0)) draft_.starting_bb = std::clamp(starting_bb, 0, 100000);
    note("Starting stack");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const long long chips = static_cast<long long>(draft_.starting_bb) * draft_.first_level().big_blind;
    ImGui::TextColored(kMutedText, "big blinds (%lld chips each)", chips);

    ImGui::Checkbox("Random personalities", &random_);
    note("Random personalities");
    tooltip("The five bots get random names, places and styles from a wider pool. Their styles are revealed when the "
            "game ends.");
}

void SetupPanel::draw_tournament(float scale) {
    const auto u = [scale](float value) { return value * scale; };
    const float column = u(150.0f);
    ImGui::SeparatorText("Blind levels");

    label("Levels last", column);
    ImGui::SetNextItemWidth(u(80.0f));
    int length = draft_.level_length;
    if (ImGui::InputInt("##length", &length, 0, 0)) draft_.level_length = std::clamp(length, 0, 1000);
    note("Levels last");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(u(120.0f));
    int unit = static_cast<int>(draft_.unit);
    if (ImGui::Combo("##unit", &unit, kUnitNames.data(), static_cast<int>(kUnitNames.size()))) {
        draft_.unit = static_cast<LevelUnit>(unit);
    }
    note("Unit");
    if (draft_.unit == LevelUnit::Minutes) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMutedText, "(blinds rise at the next hand)");
    }

    label("Schedule", column);
    ImGui::SetNextItemWidth(u(150.0f));
    int preset = static_cast<int>(draft_.preset);
    std::array<const char*, kBlindPresets.size()> names{};
    for (std::size_t i = 0; i < kBlindPresets.size(); ++i) names[i] = to_string(kBlindPresets[i]).data();
    if (ImGui::Combo("##preset", &preset, names.data(), static_cast<int>(names.size()))) {
        draft_.preset = static_cast<BlindPreset>(preset);
        rebuild_levels();
    }
    note("Schedule");
    tooltip("Standard rises gently, Turbo faster, Hyper fastest. Each starts from the big blind above.");
    ImGui::SameLine();
    if (ImGui::Button("Reset to preset")) rebuild_levels();
    note("Reset to preset");

    label("Antes", column);
    ImGui::SetNextItemWidth(u(150.0f));
    int ante = ante_index(draft_.ante_type);
    if (ImGui::Combo("##antes", &ante, kAnteNames.data(), static_cast<int>(kAnteNames.size()))) {
        draft_.ante_type = static_cast<AnteType>(ante);
        draft_.levels = with_antes(draft_.levels, draft_.ante_type, draft_.ante_from_level);
    }
    note("Antes");
    tooltip(kAnteHelp);
    if (draft_.ante_type != AnteType::None) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("from level");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u(70.0f));
        int from = draft_.ante_from_level;
        if (ImGui::InputInt("##ante_from", &from, 0, 0)) {
            draft_.ante_from_level = std::clamp(from, 1, kMaxLevels);
            draft_.levels = with_antes(draft_.levels, draft_.ante_type, draft_.ante_from_level);
        }
        note("Ante from");
    }

    label("Multiply blinds", column);
    ImGui::SetNextItemWidth(u(80.0f));
    ImGui::InputFloat("##factor", &factor_, 0.0f, 0.0f, "%.2f");
    factor_ = std::clamp(factor_, 0.1f, 10.0f);
    note("Multiply");
    tooltip(kMultiplyHelp);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        draft_.levels = scale_schedule(draft_.levels, factor_);
        if (!draft_.levels.empty()) draft_.big_blind = draft_.levels.front().big_blind;
    }
    note("Apply");
    tooltip(kMultiplyHelp);

    draw_levels(scale);
}

void SetupPanel::draw_levels(float scale) {
    const auto u = [scale](float value) { return value * scale; };
    std::vector<BlindLevel>& levels = draft_.levels;
    // Room below for the Add level row, the summary line and Start.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() + u(12.0f);
    const float table_h = std::max(u(120.0f), ImGui::GetContentRegionAvail().y - footer);
    std::optional<std::size_t> remove;
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##levels", 5, kTable, {0.0f, table_h})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, u(60.0f));
        ImGui::TableSetupColumn("Small blind", ImGuiTableColumnFlags_WidthFixed, u(130.0f));
        ImGui::TableSetupColumn("Big blind", ImGuiTableColumnFlags_WidthFixed, u(130.0f));
        ImGui::TableSetupColumn("Ante", ImGuiTableColumnFlags_WidthFixed, u(130.0f));
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, u(40.0f));
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            BlindLevel& level = levels[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            // Typed values are held to 0..kMaxLevelBigBlind. "Apply" can still scale
            // rows above it (up to INT_MAX); validate() refuses those, and "Add level"
            // is disabled once the last big blind reaches the cap.
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt("##sb", &level.small_blind, 0, 0)) {
                level.small_blind = std::clamp(level.small_blind, 0, kMaxLevelBigBlind);
            }
            note_level(i, "SB");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt("##bb", &level.big_blind, 0, 0)) {
                level.big_blind = std::clamp(level.big_blind, 0, kMaxLevelBigBlind);
                if (i == 0) draft_.big_blind = level.big_blind;
            }
            note_level(i, "BB");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt("##ante", &level.ante, 0, 0)) {
                level.ante = std::clamp(level.ante, 0, kMaxLevelBigBlind);
                level.ante_type = level.ante > 0 ? draft_.ante_type : AnteType::None;
            }
            note_level(i, "ante");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(levels.size() <= 1);
            if (ImGui::Button("x")) remove = i;
            ImGui::EndDisabled();
            note_level(i, "remove");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove) {
        levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(*remove));
        draft_.big_blind = levels.front().big_blind;  // Level 1 and the big blind stay one value
    }

    // A level can be added only below the largest big blind allowed; the step is
    // worked out in doubles, which no big blind can overflow.
    const int last = levels.empty() ? draft_.big_blind : levels.back().big_blind;
    ImGui::BeginDisabled(static_cast<int>(levels.size()) >= kMaxLevels || last >= kMaxLevelBigBlind);
    if (ImGui::Button("Add level")) {
        int bb = tidy_chips(last * 1.25);
        if (bb <= last) bb = tidy_chips(last + std::max(1.0, last / 4.0) + 1.0);
        if (bb <= last) bb = last + 1;  // last < kMaxLevelBigBlind here
        // The new level's ante follows the rule; edited antes above it stay as they are.
        const int index = static_cast<int>(levels.size()) + 1;
        const std::vector<BlindLevel> one = with_antes({BlindLevel{small_blind_for(bb), bb, 0, AnteType::None}},
                                                       draft_.ante_type, draft_.ante_from_level - index + 1);
        levels.push_back(one.front());
    }
    ImGui::EndDisabled();
    note("Add level");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMutedText, "%zu levels; the last one lasts until the end.", levels.size());
}

}  // namespace poker
