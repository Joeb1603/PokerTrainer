#include "core/card.h"

namespace poker {
namespace {

constexpr std::string_view kRankChars = "23456789TJQKA";
constexpr std::string_view kSuitChars = "cdhs";

constexpr char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr char to_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

std::optional<Card> Card::parse(std::string_view text) {
    if (text.size() != 2) return std::nullopt;

    const auto rank = kRankChars.find(to_upper(text[0]));
    const auto suit = kSuitChars.find(to_lower(text[1]));
    if (rank == std::string_view::npos || suit == std::string_view::npos) return std::nullopt;

    return Card(static_cast<Rank>(rank), static_cast<Suit>(suit));
}

std::string Card::to_string() const {
    return {kRankChars[static_cast<std::size_t>(rank())], kSuitChars[static_cast<std::size_t>(suit())]};
}

}  // namespace poker
