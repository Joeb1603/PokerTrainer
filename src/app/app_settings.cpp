#include "app/app_settings.h"

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace poker {
namespace {

constexpr const char* kRandomPersonalities = "random_personalities";

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
    } catch (...) {
        return AppSettings{};
    }
    return settings;
}

bool save_app_settings(const std::filesystem::path& path, const AppSettings& settings) noexcept {
    try {
        nlohmann::json value = read_object(path);
        value[kRandomPersonalities] = settings.random_personalities;

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

}  // namespace poker
