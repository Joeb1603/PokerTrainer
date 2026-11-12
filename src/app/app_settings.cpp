#include "app/app_settings.h"

#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace poker {
namespace {

constexpr const char* kRandomPersonalities = "random_personalities";
constexpr const char* kGameSetup = "game_setup";

nlohmann::json setup_json(const GameSetup& setup) {
    nlohmann::json out = nlohmann::json::object();
    // The same names as to_string and the parse_* functions (blind_schedule.h).
    out["mode"] = std::string(to_string(setup.mode));
    out["big_blind"] = setup.big_blind;
    out["starting_bb"] = setup.starting_bb;
    out["unit"] = std::string(to_string(setup.unit));
    out["level_length"] = setup.level_length;
    out["preset"] = std::string(to_string(setup.preset));
    out["ante"] = std::string(to_string(setup.ante_type));
    out["ante_from_level"] = setup.ante_from_level;
    if (!setup.levels.empty()) {
        nlohmann::json levels = nlohmann::json::array();
        for (const BlindLevel& level : setup.levels) {
            levels.push_back(nlohmann::json::array({level.small_blind, level.big_blind, level.has_ante() ? level.ante : 0}));
        }
        out["levels"] = std::move(levels);
    }
    return out;
}

// Every key must be present and well typed, and the whole must pass validate().
std::optional<GameSetup> setup_from(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto text = [&](const char* key) -> std::optional<std::string> {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    const auto number = [&](const char* key) -> std::optional<int> {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_number_integer()) return std::nullopt;
        const auto n = it->get<long long>();
        if (n < -1'000'000'000LL || n > 1'000'000'000LL) return std::nullopt;
        return static_cast<int>(n);
    };
    GameSetup setup;
    const auto mode = text("mode");
    const auto big_blind = number("big_blind");
    const auto starting_bb = number("starting_bb");
    const auto unit = text("unit");
    const auto length = number("level_length");
    const auto preset = text("preset");
    const auto ante = text("ante");
    const auto from = number("ante_from_level");
    if (!mode || !big_blind || !starting_bb || !unit || !length || !preset || !ante || !from) return std::nullopt;
    const auto parsed_mode = parse_game_mode(*mode);
    const auto parsed_unit = parse_level_unit(*unit);
    const auto parsed_preset = parse_blind_preset(*preset);
    const auto parsed_ante = parse_ante_type(*ante);
    if (!parsed_mode || !parsed_unit || !parsed_preset || !parsed_ante) return std::nullopt;
    setup.mode = *parsed_mode;
    setup.big_blind = *big_blind;
    setup.starting_bb = *starting_bb;
    setup.unit = *parsed_unit;
    setup.level_length = *length;
    setup.preset = *parsed_preset;
    setup.ante_type = *parsed_ante;
    setup.ante_from_level = *from;
    if (const auto it = value.find("levels"); it != value.end()) {
        if (!it->is_array()) return std::nullopt;
        for (const nlohmann::json& row : *it) {
            if (!row.is_array() || row.size() != 3) return std::nullopt;
            std::array<int, 3> cells{};
            for (std::size_t i = 0; i < 3; ++i) {
                if (!row[i].is_number_integer()) return std::nullopt;
                const auto n = row[i].get<long long>();
                if (n < 0 || n > 1'000'000'000LL) return std::nullopt;
                cells[i] = static_cast<int>(n);
            }
            setup.levels.push_back(
                BlindLevel{cells[0], cells[1], cells[2], cells[2] > 0 ? setup.ante_type : AnteType::None});
        }
    }
    if (validate(setup)) return std::nullopt;
    return setup;
}

nlohmann::json read_object(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nlohmann::json::object();
    nlohmann::json value = nlohmann::json::parse(in, nullptr, false);
    return value.is_object() ? value : nlohmann::json::object();
}

}  // namespace

AppSettings load_app_settings(const std::filesystem::path& path) noexcept {
    AppSettings settings;
    try {
        const nlohmann::json value = read_object(path);
        const auto it = value.find(kRandomPersonalities);
        if (it != value.end() && it->is_boolean()) settings.random_personalities = it->get<bool>();
        if (const auto setup = value.find(kGameSetup); setup != value.end()) {
            if (auto parsed = setup_from(*setup)) settings.game_setup = std::move(*parsed);
        }
    } catch (...) {
        return AppSettings{};
    }
    return settings;
}

bool update_app_settings(const std::filesystem::path& path, const AppSettingsUpdate& update) noexcept {
    try {
        nlohmann::json value = read_object(path);
        if (update.random_personalities) value[kRandomPersonalities] = *update.random_personalities;
        if (update.game_setup) value[kGameSetup] = setup_json(*update.game_setup);

        // Written beside the file and renamed over it, so a failed write never
        // leaves half a file behind.
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << value.dump(2) << '\n';
            if (!out) return false;
        }
        std::error_code error;
        std::filesystem::rename(temp, path, error);
        if (error) {
            std::filesystem::remove(temp, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool save_app_settings(const std::filesystem::path& path, const AppSettings& settings) noexcept {
    return update_app_settings(path, AppSettingsUpdate{settings.random_personalities, settings.game_setup});
}

}  // namespace poker
