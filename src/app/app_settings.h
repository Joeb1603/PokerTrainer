#pragma once

#include <filesystem>
#include <optional>

#include "controller/blind_schedule.h"

namespace poker {

// Front-end preferences remembered between runs, in a small JSON file. Only the
// app reads or writes it; the controller just receives the values.
struct AppSettings {
    bool random_personalities = false;
    GameSetup game_setup = default_freezeout();  // The last game started from the setup screen
};

// poker_trainer_settings.json in the working folder, beside logs/.
inline constexpr const char* kDefaultSettingsFile = "poker_trainer_settings.json";

// A missing, unreadable or corrupt file gives the defaults; so does a key that is
// missing or of the wrong type, and a game setup that is incomplete or that
// validate() rejects. Never throws.
AppSettings load_app_settings(const std::filesystem::path& path) noexcept;

// The settings to change; an absent field leaves the file's value as it is.
struct AppSettingsUpdate {
    std::optional<bool> random_personalities = std::nullopt;
    std::optional<GameSetup> game_setup = std::nullopt;
};

// Writes only the fields given, keeping every other key a valid file already holds
// exactly as it was, even a game setup this version cannot read (so the sidebar
// checkbox never replaces it). Returns false, without throwing, if the file cannot
// be written.
bool update_app_settings(const std::filesystem::path& path, const AppSettingsUpdate& update) noexcept;

// Writes both settings, keeping any other keys; the same as an update of both.
bool save_app_settings(const std::filesystem::path& path, const AppSettings& settings) noexcept;

}  // namespace poker
