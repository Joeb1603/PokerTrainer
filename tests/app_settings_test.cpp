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
