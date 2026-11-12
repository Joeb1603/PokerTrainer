#include "controller/bot_names.h"

#include <utility>

#include "core/random.h"

namespace poker {
namespace {

constexpr std::array<std::string_view, 40> kNames{
    "Marta", "Jonas", "Priya", "Diego",  "Aiko",  "Tomas", "Lena",  "Omar",   "Freya", "Kofi",
    "Ines",  "Ravi",  "Sofia", "Mateo",  "Yuki",  "Nadia", "Felix", "Amara",  "Lars",  "Chloe",
    "Hugo",  "Leila", "Emil",  "Zara",   "Tariq", "Mira",  "Oskar", "Ana",    "Bruno", "Elif",
    "Dmitri", "Noor", "Pablo", "Saskia", "Kenji", "Ada",   "Luca",  "Ingrid", "Femi",  "Rosa"};

}  // namespace

std::span<const std::string_view> bot_name_pool() { return kNames; }

std::array<std::string, 5> draw_bot_names(std::mt19937_64& rng) {
    // A partial Fisher-Yates over the pool's indices.
    std::array<std::size_t, kNames.size()> order{};
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::array<std::string, 5> names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t j = i + static_cast<std::size_t>(uniform_below(rng, order.size() - i));
        std::swap(order[i], order[j]);
        names[i] = std::string(kNames[order[i]]);
    }
    return names;
}

}  // namespace poker
