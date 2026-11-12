#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "app/app_settings.h"

using namespace poker;
namespace fs = std::filesystem;

namespace {

class AppSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("poker_settings_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }
    void write(const std::string& text) const {
        std::ofstream out(file(), std::ios::binary);
        out << text;
    }
    fs::path file() const { return dir_ / "settings.json"; }

    fs::path dir_;
};

}  // namespace

TEST_F(AppSettingsTest, AMissingFileGivesTheDefaults) {
    EXPECT_FALSE(load_app_settings(file()).random_personalities);
    EXPECT_FALSE(fs::exists(file()));  // Loading never creates it
}

TEST_F(AppSettingsTest, ACorruptFileOrAWrongTypeGivesTheDefaults) {
    for (const char* text : {"{not json", "[true]", "", "{\"random_personalities\": \"yes\"}", "42"}) {
        write(text);
        EXPECT_FALSE(load_app_settings(file()).random_personalities) << text;
    }
}

TEST_F(AppSettingsTest, RoundTripsAndKeepsOtherKeys) {
    write(R"({"random_personalities": false, "theme": "dark", "volume": 3})");
    ASSERT_TRUE(save_app_settings(file(), AppSettings{.random_personalities = true}));
    EXPECT_TRUE(load_app_settings(file()).random_personalities);

    std::ifstream in(file());
    const nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved["theme"], "dark");
    EXPECT_EQ(saved["volume"], 3);
    EXPECT_FALSE(fs::exists(dir_ / "settings.json.tmp"));

    ASSERT_TRUE(save_app_settings(file(), AppSettings{}));
    EXPECT_FALSE(load_app_settings(file()).random_personalities);
}

TEST_F(AppSettingsTest, SavingOverACorruptFileStartsAfresh) {
    write("{not json");
    ASSERT_TRUE(save_app_settings(file(), AppSettings{.random_personalities = true}));
    EXPECT_TRUE(load_app_settings(file()).random_personalities);
}

TEST_F(AppSettingsTest, AnUnwritablePlaceFailsWithoutThrowing) {
    EXPECT_FALSE(save_app_settings(dir_ / "no_such_folder" / "settings.json", AppSettings{.random_personalities = true}));
}

TEST_F(AppSettingsTest, TheGameSetupDefaultsToTheClassicFreezeout) {
    EXPECT_EQ(load_app_settings(file()).game_setup, default_freezeout());
    write(R"({"random_personalities": true})");  // A file from before game setups
    const AppSettings old = load_app_settings(file());
    EXPECT_TRUE(old.random_personalities);
    EXPECT_EQ(old.game_setup, default_freezeout());
}

TEST_F(AppSettingsTest, AGameSetupRoundTrips) {
    GameSetup tournament = default_tournament();
    tournament.unit = LevelUnit::Minutes;
    tournament.level_length = 7;
    tournament.preset = BlindPreset::Turbo;
    tournament.ante_type = AnteType::PerPlayer;
    tournament.ante_from_level = 3;
    tournament.starting_bb = 75;
    ASSERT_TRUE(save_app_settings(file(), AppSettings{.random_personalities = false, .game_setup = tournament}));
    EXPECT_EQ(load_app_settings(file()).game_setup, tournament);

    // Edited levels are kept as written, with the setup's ante type.
    tournament.levels = {{10, 20, 0, AnteType::None}, {25, 50, 5, AnteType::PerPlayer}};
    ASSERT_TRUE(save_app_settings(file(), AppSettings{.game_setup = tournament}));
    EXPECT_EQ(load_app_settings(file()).game_setup, tournament);

    std::ifstream in(file());
    const nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved["game_setup"]["mode"], "Tournament");
    EXPECT_EQ(saved["game_setup"]["ante"], "PerPlayer");
    EXPECT_EQ(saved["game_setup"]["levels"][1], nlohmann::json::array({25, 50, 5}));
}

TEST_F(AppSettingsTest, ABadGameSetupGivesTheDefaultAndKeepsTheRest) {
    const char* bad[] = {
        R"({"random_personalities": true, "game_setup": "tournament"})",
        R"({"random_personalities": true, "game_setup": {"mode": "tournament"}})",
        R"({"random_personalities": true, "game_setup": {"mode": "cash", "big_blind": 20, "starting_bb": 100, "unit": "hands",
            "level_length": 10, "preset": "standard", "ante": "none", "ante_from_level": 4}})",
        R"({"random_personalities": true, "game_setup": {"mode": "freezeout", "big_blind": 1, "starting_bb": 100,
            "unit": "hands", "level_length": 10, "preset": "standard", "ante": "none", "ante_from_level": 4}})",
        R"({"random_personalities": true, "game_setup": {"mode": "tournament", "big_blind": 20, "starting_bb": 100,
            "unit": "hands", "level_length": 10, "preset": "standard", "ante": "bb", "ante_from_level": 4,
            "levels": [[10, 20, 0], [5, 10, 0]]}})",
        R"({"random_personalities": true, "game_setup": {"mode": "tournament", "big_blind": 20.5, "starting_bb": 100,
            "unit": "hands", "level_length": 10, "preset": "standard", "ante": "bb", "ante_from_level": 4}})",
        // Level 1 disagrees with the big blind.
        R"({"random_personalities": true, "game_setup": {"mode": "tournament", "big_blind": 20, "starting_bb": 100,
            "unit": "hands", "level_length": 10, "preset": "standard", "ante": "bb", "ante_from_level": 4,
            "levels": [[25, 50, 0], [50, 100, 0]]}})",
    };
    for (const char* text : bad) {
        write(text);
        const AppSettings settings = load_app_settings(file());
        EXPECT_EQ(settings.game_setup, default_freezeout()) << text;
        EXPECT_TRUE(settings.random_personalities) << text;
    }
}

TEST_F(AppSettingsTest, AnUpdateKeepsAGameSetupItCannotRead) {
    // A setup that validate() rejects (or a newer version wrote) stays in the file when
    // only the checkbox is saved, along with any other key.
    const nlohmann::json unreadable = {{"mode", "tournament"}, {"preset", "superturbo"}, {"levels", {{1, 2, 0}}}};
    std::ofstream(file(), std::ios::binary)
        << nlohmann::json{{"random_personalities", false}, {"game_setup", unreadable}, {"other", 7}}.dump();

    ASSERT_TRUE(update_app_settings(file(), AppSettingsUpdate{.random_personalities = true}));
    std::ifstream in(file(), std::ios::binary);
    const nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved["random_personalities"], true);
    EXPECT_EQ(saved["game_setup"], unreadable);
    EXPECT_EQ(saved["other"], 7);
    EXPECT_EQ(load_app_settings(file()).game_setup, default_freezeout());  // Still unreadable, so the default
}

TEST_F(AppSettingsTest, AnUpdateOfTheSetupKeepsTheCheckbox) {
    ASSERT_TRUE(update_app_settings(file(), AppSettingsUpdate{.random_personalities = true}));
    EXPECT_EQ(load_app_settings(file()).game_setup, default_freezeout());

    ASSERT_TRUE(update_app_settings(file(), AppSettingsUpdate{.game_setup = default_tournament()}));
    const AppSettings settings = load_app_settings(file());
    EXPECT_TRUE(settings.random_personalities);
    EXPECT_EQ(settings.game_setup, default_tournament());

    ASSERT_TRUE(update_app_settings(file(), AppSettingsUpdate{}));  // Nothing given: nothing changes
    EXPECT_EQ(load_app_settings(file()).game_setup, default_tournament());
}

TEST_F(AppSettingsTest, AnUpdateToAMissingFileWritesOnlyWhatIsGiven) {
    ASSERT_TRUE(update_app_settings(file(), AppSettingsUpdate{.random_personalities = true}));
    std::ifstream in(file(), std::ios::binary);
    const nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved, (nlohmann::json{{"random_personalities", true}}));
}
