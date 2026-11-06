#pragma once

// Text forms of boards, weighted ranges and numbers for the command-line tools.
// Header-only and tools-only: the trainer itself never parses these.

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/card.h"
#include "range/combos.h"
#include "range/weighted_range.h"
#include "strategy/preflop_charts.h"

namespace poker::tools {

inline std::string_view trim_text(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// The whole of `text` as a number, or nothing: no sign games, no trailing junk.
inline std::optional<int> parse_int(std::string_view text) {
    text = trim_text(text);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

inline std::optional<double> parse_double(std::string_view text) {
    text = trim_text(text);
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Comma-separated numbers, e.g. "25,50,75". Throws std::invalid_argument.
inline std::vector<double> parse_number_list(std::string_view text) {
    std::vector<double> values;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::optional<double> value = parse_double(text.substr(0, comma));
        if (!value) throw std::invalid_argument("bad number in list: '" + std::string(text.substr(0, comma)) + "'");
        values.push_back(*value);
        if (comma == std::string_view::npos) return values;
        text = text.substr(comma + 1);
    }
}

// Two-character cards run together, optionally separated by spaces or commas:
// "Kh9d5c3s2h" or "Kh 9d 5c". 3 to 5 distinct cards. Throws std::invalid_argument.
inline std::vector<Card> parse_board(std::string_view text) {
    std::vector<Card> board;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == ',') {
            ++i;
            continue;
        }
        const std::optional<Card> card = i + 2 <= text.size() ? Card::parse(text.substr(i, 2)) : std::nullopt;
        if (!card) throw std::invalid_argument("bad card in board: '" + std::string(text.substr(i, 2)) + "'");
        if ((card_mask(board) & card_mask(*card)) != 0) {
            throw std::invalid_argument("board repeats " + card->to_string());
        }
        board.push_back(*card);
        i += 2;
    }
    if (board.size() < 3 || board.size() > 5) throw std::invalid_argument("board needs 3 to 5 cards");
    return board;
}

// HandRange syntax where each comma-separated token may end in ":weight", e.g.
// "QQ+, AKs, AKo:0.5, A5s-A2s:0.25". A token without one has weight 1. Weights
// must be finite and non-negative; a later token overrides an earlier one for the
// classes they share, so "TT+, AA:0" is TT-KK. Throws std::invalid_argument.
inline WeightedRange parse_weighted_range(std::string_view text) {
    WeightedRange range;
    if (trim_text(text).empty()) return range;
    while (true) {
        const std::size_t comma = text.find(',');
        std::string_view token = trim_text(text.substr(0, comma));
        if (token.empty()) throw std::invalid_argument("range has an empty token");

        float weight = 1.0f;
        if (const std::size_t colon = token.find(':'); colon != std::string_view::npos) {
            const std::optional<double> value = parse_double(token.substr(colon + 1));
            if (!value || *value < 0.0) {
                throw std::invalid_argument("bad weight in range token '" + std::string(token) + "'");
            }
            weight = static_cast<float>(*value);
            token = trim_text(token.substr(0, colon));
        }

        HandRange classes;
        try {
            classes = HandRange::parse(token);
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument("bad range token '" + std::string(token) + "'");
        }
        if (classes.empty()) throw std::invalid_argument("bad range token '" + std::string(token) + "'");
        for (const HandClass hand : hand_classes_by_strength()) {
            if (classes.contains(hand)) range.set_class(hand, weight);
        }

        if (comma == std::string_view::npos) return range;
        text = text.substr(comma + 1);
    }
}

}  // namespace poker::tools
