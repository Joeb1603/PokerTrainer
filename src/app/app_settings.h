#pragma once

#include <filesystem>

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

// Writes the settings, keeping any other keys a valid file already holds. Returns
// false, without throwing, if the file cannot be written.
bool save_app_settings(const std::filesystem::path& path, const AppSettings& settings) noexcept;

}  // namespace poker
