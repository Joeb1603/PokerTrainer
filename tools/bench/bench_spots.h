#pragma once

// Benchmark spots for tools/advisor_bench and tests/bench_test.cpp: loading
// tools/bench/river_spots.json or turn_spots.json and replaying a spot's script
// through GameEngine.
// Header-only and tools-only; the syntax is documented in tools/bench/README.md.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../range_text.h"
#include "bots/gto_heuristic_engine.h"
#include "core/card.h"
#include "engine/game_engine.h"
#include "range/combos.h"
#include "range/weighted_range.h"

namespace poker::bench {

inline constexpr std::array<std::string_view, 4> kStreetKeys{"pre", "flop", "turn", "river"};

// One scripted action: "POS:F", "POS:X", "POS:C", "POS:A", "POS:B<n>", "POS:R<n>",
// where n is the street total in chips ("R50" raises to 50) or, with a trailing
// %, a fraction of the pot after calling ("B33%"), sized as the advisor sizes.
struct ScriptAction {
    Position position = Position::UTG;
    ActionType type = ActionType::Fold;
    int amount = 0;                     // Street total, when not a percentage
    std::optional<double> pot_fraction;  // "B33%" is 0.33
};

struct BenchSpot {
    std::string id;
    std::string description;
    std::string pot_type;
    std::string family;
    std::string line;
    Street street = Street::River;  // The street of the hero's decision (the file's)
    std::vector<Card> board;  // Five cards on the river, four on the turn
    // Preflop, flop, turn, river, up to the hero's decision; empty after its street.
    std::array<std::vector<ScriptAction>, 4> script;
    std::array<std::string, 4> script_text;
    bool hero_ip = false;
    std::array<std::string, 2> range_text;  // OOP, IP
    std::array<WeightedRange, 2> ranges;    // As parsed: board cards not yet removed
    std::array<Position, 2> players{};      // OOP, IP, from the preflop script
    int pot = 0;                            // Every chip in the middle at the hero's decision
    std::array<int, 2> stacks{};            // OOP, IP stacks behind at the hero's decision

    int hero() const { return hero_ip ? 1 : 0; }
    // "OOP first", or "IP vs 33%" from the bet the hero faces on the decision street.
    std::string role() const {
        if (!hero_ip) return "OOP first";
        const std::vector<ScriptAction>& last = script[static_cast<std::size_t>(street)];
        if (last.empty() || !last.back().pot_fraction) return "IP";
        return "IP vs " + std::to_string(static_cast<int>(std::lround(*last.back().pot_fraction * 100.0))) + "%";
    }
};

struct BenchFile {
    Street street = Street::River;  // "street": "turn" or "river" (the default)
    int stack = 2000;
    int small_blind = 10;
    int big_blind = 20;
    std::vector<BenchSpot> spots;
};

inline std::optional<Position> parse_position(std::string_view text) {
    for (int p = 0; p < 6; ++p) {
        const auto position = static_cast<Position>(p);
        if (to_string(position) == text) return position;
    }
    return std::nullopt;
}

// Space-separated actions. Throws std::invalid_argument.
inline std::vector<ScriptAction> parse_street_script(std::string_view text) {
    std::vector<ScriptAction> actions;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        const std::size_t end = std::min(text.find(' ', i), text.size());
        const std::string_view token = text.substr(i, end - i);
        i = end;
        const std::string bad = "bad script action '" + std::string(token) + "'";
        const std::size_t colon = token.find(':');
        if (colon == std::string_view::npos || colon + 1 >= token.size()) throw std::invalid_argument(bad);
        const std::optional<Position> position = parse_position(token.substr(0, colon));
        if (!position) throw std::invalid_argument(bad);
        ScriptAction action;
        action.position = *position;
        const char kind = token[colon + 1];
        std::string_view rest = token.substr(colon + 2);
        switch (kind) {
            case 'F': action.type = ActionType::Fold; break;
            case 'X': action.type = ActionType::Check; break;
            case 'C': action.type = ActionType::Call; break;
            case 'A': action.type = ActionType::AllIn; break;
            case 'B': action.type = ActionType::Bet; break;
            case 'R': action.type = ActionType::Raise; break;
            default: throw std::invalid_argument(bad);
        }
        const bool sized = action.type == ActionType::Bet || action.type == ActionType::Raise;
        if (sized != !rest.empty()) throw std::invalid_argument(bad);
        if (sized) {
            if (rest.back() == '%') {
                const std::optional<double> pct = tools::parse_double(rest.substr(0, rest.size() - 1));
                if (!pct || *pct <= 0.0) throw std::invalid_argument(bad);
                action.pot_fraction = *pct / 100.0;
            } else {
                const std::optional<int> amount = tools::parse_int(rest);
                if (!amount || *amount <= 0) throw std::invalid_argument(bad);
                action.amount = *amount;
            }
        }
        actions.push_back(action);
    }
    return actions;
}

// Postflop order: SB first, the button last.
inline int postflop_rank(Position position) {
    switch (position) {
        case Position::SB: return 0;
        case Position::BB: return 1;
        case Position::UTG: return 2;
        case Position::HJ: return 3;
        case Position::CO: return 4;
        case Position::BTN: return 5;
    }
    return 6;
}

// The two players who do not fold preflop, OOP first. Throws std::invalid_argument
// unless there are exactly two.
inline std::array<Position, 2> heads_up_players(const std::vector<ScriptAction>& preflop) {
    std::vector<Position> live;
    for (const ScriptAction& action : preflop) {
        if (std::find(live.begin(), live.end(), action.position) == live.end()) live.push_back(action.position);
    }
    for (const ScriptAction& action : preflop) {
        if (action.type == ActionType::Fold) live.erase(std::remove(live.begin(), live.end(), action.position), live.end());
    }
    if (live.size() != 2) throw std::invalid_argument("the preflop script must leave exactly two players");
    if (postflop_rank(live[0]) > postflop_rank(live[1])) std::swap(live[0], live[1]);
    return {live[0], live[1]};
}

inline const nlohmann::json& field(const nlohmann::json& object, const char* key) {
    if (!object.contains(key)) throw std::invalid_argument(std::string("missing field '") + key + "'");
    return object.at(key);
}

// `street` is the file's decision street: the board has street + 2 cards, and the
// script keys after it may be missing or empty.
inline BenchSpot parse_spot(const nlohmann::json& entry, Street street) {
    BenchSpot spot;
    spot.street = street;
    spot.id = field(entry, "id").get<std::string>();
    try {
        spot.description = field(entry, "description").get<std::string>();
        spot.pot_type = field(entry, "pot_type").get<std::string>();
        spot.family = field(entry, "family").get<std::string>();
        spot.line = field(entry, "line").get<std::string>();
        spot.board = tools::parse_board(field(entry, "board").get<std::string>());
        const auto decision = static_cast<std::size_t>(street);
        if (spot.board.size() != decision + 2) {
            throw std::invalid_argument("the board needs " + std::to_string(decision + 2) + " cards");
        }
        for (std::size_t s = 0; s < 4; ++s) {
            const std::string key(kStreetKeys[s]);
            if (s > decision && !entry.contains(key)) continue;
            spot.script_text[s] = field(entry, key.c_str()).get<std::string>();
            spot.script[s] = parse_street_script(spot.script_text[s]);
            if (s > decision && !spot.script[s].empty()) {
                throw std::invalid_argument("no actions allowed after the decision street (" + key + ")");
            }
        }
        const std::string hero = field(entry, "hero").get<std::string>();
        if (hero != "oop" && hero != "ip") throw std::invalid_argument("hero must be oop or ip");
        spot.hero_ip = hero == "ip";
        spot.range_text[0] = field(entry, "oop_range").get<std::string>();
        spot.range_text[1] = field(entry, "ip_range").get<std::string>();
        for (std::size_t p = 0; p < 2; ++p) spot.ranges[p] = tools::parse_weighted_range(spot.range_text[p]);
        spot.players = heads_up_players(spot.script[0]);
        spot.pot = field(entry, "pot").get<int>();
        const nlohmann::json& stacks = field(entry, "stacks");
        if (!stacks.is_array() || stacks.size() != 2) throw std::invalid_argument("stacks must be [oop, ip]");
        spot.stacks = {stacks[0].get<int>(), stacks[1].get<int>()};
    } catch (const std::exception& error) {
        throw std::invalid_argument("spot " + spot.id + ": " + error.what());
    }
    return spot;
}

// Throws std::invalid_argument (or nlohmann::json's exceptions) for a bad file.
inline BenchFile parse_bench_file(const std::string& text) {
    const nlohmann::json root = nlohmann::json::parse(text);
    BenchFile file;
    if (root.contains("street")) {
        const std::string street = root.at("street").get<std::string>();
        if (street == "turn") {
            file.street = Street::Turn;
        } else if (street != "river") {
            throw std::invalid_argument("street must be turn or river");
        }
    }
    file.stack = field(root, "stack").get<int>();
    file.small_blind = field(root, "small_blind").get<int>();
    file.big_blind = field(root, "big_blind").get<int>();
    for (const nlohmann::json& entry : field(root, "spots")) file.spots.push_back(parse_spot(entry, file.street));
    std::vector<std::string> ids;
    for (const BenchSpot& spot : file.spots) ids.push_back(spot.id);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) throw std::invalid_argument("duplicate spot id");
    return file;
}

inline BenchFile load_bench_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return parse_bench_file(text.str());
}

// Six players, button at 0: BTN 0, SB 1, BB 2, UTG 3, HJ 4, CO 5.
inline int seat_of(Position position) {
    switch (position) {
        case Position::BTN: return 0;
        case Position::SB: return 1;
        case Position::BB: return 2;
        case Position::UTG: return 3;
        case Position::HJ: return 4;
        case Position::CO: return 5;
    }
    return 0;
}

// The hero's and the villain's table index.
inline int hero_seat(const BenchSpot& spot) { return seat_of(spot.players[static_cast<std::size_t>(spot.hero())]); }
inline int villain_seat(const BenchSpot& spot) {
    return seat_of(spot.players[static_cast<std::size_t>(1 - spot.hero())]);
}

// The lowest-index combo of `range` with weight > 0 that shares no card with
// `dead`, or nothing.
inline std::optional<int> first_combo(const WeightedRange& range, CardMask dead) {
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (range.weight(combo) > 0.0f && (combo_mask(combo) & dead) == 0) return combo;
    }
    return std::nullopt;
}

// Deals the spot with the given hole cards (the other four players, and a turn
// spot's river card, get the lowest unused cards), then plays the script. Returns
// the engine at the hero's decision on the spot's street. Throws std::runtime_error
// when an action is illegal, comes out of turn, or a street ends somewhere other
// than where the script does.
inline GameEngine replay_spot(const BenchFile& file, const BenchSpot& spot, const std::array<Card, 2>& hero_cards,
                              const std::array<Card, 2>& villain_cards) {
    GameConfig config;
    config.starting_stack = file.stack;
    config.small_blind = file.small_blind;
    config.big_blind = file.big_blind;
    config.hero_seat = std::nullopt;
    config.first_button = 0;
    GameEngine engine(config);

    ScriptedDeal deal;
    deal.holes[static_cast<std::size_t>(hero_seat(spot))] = hero_cards;
    deal.holes[static_cast<std::size_t>(villain_seat(spot))] = villain_cards;
    CardMask used = card_mask(std::span<const Card>(spot.board)) | card_mask(std::span<const Card>(hero_cards)) |
                    card_mask(std::span<const Card>(villain_cards));
    if (std::popcount(used) != static_cast<int>(spot.board.size()) + 4) {
        throw std::runtime_error(spot.id + ": hole cards clash with each other or the board");
    }
    int next = 0;
    const auto spare = [&] {
        while ((used & (CardMask{1} << next)) != 0) ++next;
        used |= CardMask{1} << next;
        return Card(next);
    };
    for (std::size_t c = 0; c < 5; ++c) deal.board[c] = c < spot.board.size() ? spot.board[c] : spare();
    for (auto& hole : deal.holes) {
        if (!hole) hole = std::array<Card, 2>{spare(), spare()};
    }
    engine.start_hand(deal);

    const auto fail = [&](const std::string& message) { throw std::runtime_error(spot.id + ": " + message); };
    for (std::size_t s = 0; s <= static_cast<std::size_t>(spot.street); ++s) {
        const auto street = static_cast<Street>(s);
        if (engine.state().street != street) fail("expected " + std::string(to_string(street)) + " to start");
        for (const ScriptAction& scripted : spot.script[s]) {
            const int seat = seat_of(scripted.position);
            const PlayerView view = engine.player_view(seat);
            if (view.position != scripted.position) fail("position mismatch");
            if (!view.to_act || *view.to_act != seat) {
                fail(std::string(to_string(scripted.position)) + " acts out of turn on the " +
                     std::string(to_string(street)));
            }
            Action action{scripted.type, scripted.amount};
            if (scripted.pot_fraction) action.amount = raise_to_for_pot_fraction(view, *scripted.pot_fraction);
            if (!engine.apply_action(action)) {
                fail("illegal action by " + std::string(to_string(scripted.position)) + " on the " +
                     std::string(to_string(street)));
            }
            if (engine.state().street != street && &scripted != &spot.script[s].back()) {
                fail("the " + std::string(to_string(street)) + " ended before its script did");
            }
        }
    }
    const std::string decision(to_string(spot.street));
    if (engine.state().street != spot.street) fail("the hand left the " + decision + " before the hero's decision");
    if (engine.state().to_act != hero_seat(spot)) fail("the hero is not the one to act on the " + decision);
    return engine;
}

}  // namespace poker::bench
