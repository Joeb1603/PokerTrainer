#include "logging/hand_logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace poker {
namespace {

// Ordered, because the schema's field order is part of what makes it readable.
using Json = nlohmann::ordered_json;

constexpr std::array<const char*, 4> kStreetKeys{"pre", "flop", "turn", "riv"};

double one_decimal(double value) { return std::round(value * 10.0) / 10.0; }

// Rationale text must never be able to break a log line or abort a session.
std::string to_line(const Json& json) {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string action_code(ActionType type, bool all_in, int chips_added, int street_total) {
    if (type == ActionType::Fold) return "F";
    if (type == ActionType::Check) return "X";
    if (all_in) return "A" + std::to_string(street_total);
    if (type == ActionType::Call) return "C" + std::to_string(chips_added);
    return (type == ActionType::Bet ? "B" : "R") + std::to_string(street_total);
}

std::string cards_text(const std::array<Card, 2>& cards) { return cards[0].to_string() + cards[1].to_string(); }

std::ofstream open_for_append(const std::filesystem::path& path) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    // Binary, so lines end in \n on every platform.
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open log file: " + path.string());
    return out;
}

void write_line(std::ofstream& out, const std::string& line) {
    out << line << '\n';
    out.flush();
    if (!out) throw std::runtime_error("Failed to write log file");
}

bool can_aggress(const LegalActions& legal) {
    return legal.allows(ActionType::Bet) || legal.allows(ActionType::Raise);
}

// Uses the model's own EVs: preferring another size is advice, not a mistake by itself.
std::optional<double> ev_of_action(const HintRecommendation& hint, const PlayerView& view, const Action& action) {
    const LegalActions& legal = view.legal;
    const EvEstimates& ev_table = hint.base_ev;
    switch (action.type) {
        case ActionType::Fold:
            return ev_table.fold;
        case ActionType::Check:
        case ActionType::Call:
            return ev_table.check_call;
        case ActionType::AllIn:
            if (!can_aggress(legal)) return ev_table.check_call;  // An all-in call
            break;
        case ActionType::Bet:
        case ActionType::Raise:
            break;
    }

    const int to = action.type == ActionType::AllIn ? legal.max_raise_to : action.amount;
    std::optional<double> nearest;
    int nearest_distance = std::numeric_limits<int>::max();
    const auto consider = [&](std::optional<int> size, std::optional<double> ev) {
        if (!size || !ev) return;
        const int distance = std::abs(*size - to);
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = ev;
        }
    };
    consider(legal.max_raise_to, ev_table.all_in);
    consider(hint.raise_100_to, ev_table.raise_100);
    consider(hint.raise_75_to, ev_table.raise_75);
    consider(hint.raise_50_to, ev_table.raise_50);
    consider(hint.raise_25_to, ev_table.raise_25);
    return nearest;
}

enum class ActionClass { Fold, Passive, Aggressive };

ActionClass classify(const Action& action, const LegalActions& legal) {
    switch (action.type) {
        case ActionType::Fold: return ActionClass::Fold;
        case ActionType::Check:
        case ActionType::Call: return ActionClass::Passive;
        case ActionType::AllIn: return can_aggress(legal) ? ActionClass::Aggressive : ActionClass::Passive;
        case ActionType::Bet:
        case ActionType::Raise: return ActionClass::Aggressive;
    }
    return ActionClass::Fold;
}

std::string taken_code(const PlayerView& view, const Action& action) {
    const PublicSeat& seat = view.seats[view.seat];
    const LegalActions& legal = view.legal;
    switch (action.type) {
        case ActionType::Fold:
        case ActionType::Check:
            return action_code(action.type, false, 0, seat.street_bet);
        case ActionType::Call:
            return action_code(ActionType::Call, legal.call_amount == seat.stack, legal.call_amount,
                               seat.street_bet + legal.call_amount);
        case ActionType::AllIn:
            return action_code(ActionType::AllIn, true, seat.stack, seat.street_bet + seat.stack);
        case ActionType::Bet:
        case ActionType::Raise:
            return action_code(action.type, action.amount == seat.street_bet + seat.stack,
                               action.amount - seat.street_bet, action.amount);
    }
    return {};
}

Json mistake_json(const HeroMistake& mistake) {
    return Json{{"st", kStreetKeys[static_cast<std::size_t>(mistake.street)]},
                {"did", mistake.taken},
                {"rec", mistake.recommended},
                {"eq", one_decimal(mistake.equity_pct)},
                {"odds", one_decimal(mistake.pot_odds_pct)},
                {"mdf", one_decimal(mistake.mdf_pct)},
                {"ev_loss", one_decimal(mistake.ev_loss)},
                {"why", mistake.rationale}};
}

}  // namespace

std::string make_session_id() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::random_device entropy;
    return std::format("{:%Y%m%dT%H%M%SZ}-{:04x}", now, entropy() & 0xFFFFu);
}

std::optional<HeroMistake> assess_hero_decision(const PlayerView& view, const HintRecommendation& hint,
                                                const Action& taken) {
    const std::optional<double> taken_ev = ev_of_action(hint, view, taken);
    const std::optional<double> best_ev = ev_of_action(hint, view, hint.action);
    const double loss = taken_ev && best_ev ? std::max(0.0, *best_ev - *taken_ev) : 0.0;

    const bool folded_for_free = taken.type == ActionType::Fold && view.legal.allows(ActionType::Check);
    const bool lost_enough = loss >= kMistakeEvGapBigBlinds * view.big_blind;
    const bool unknown_and_different =
        (!taken_ev || !best_ev) && classify(taken, view.legal) != classify(hint.action, view.legal);
    if (!folded_for_free && !lost_enough && !unknown_and_different) return std::nullopt;

    return HeroMistake{view.street,      taken_code(view, taken), hint.best_action, hint.equity_pct,
                       hint.pot_odds_pct, hint.mdf_pct,            loss,             hint.rationale};
}

std::string serialize_hand(const GameState& state, const HandLogContext& context) {
    if (!state.last_result || state.phase == GamePhase::Betting) {
        throw std::logic_error("serialize_hand: no finished hand");
    }
    const HandResult& result = *state.last_result;

    Json root;
    root["session_id"] = context.session_id;
    root["h"] = state.hand_number;
    root["sb"] = context.small_blind;
    root["bb"] = context.big_blind;
    root["btn"] = state.button;
    root["post"] = Json{{"sb", Json{{"s", state.small_blind_seat}, {"amt", state.small_blind_posted}}},
                        {"bb", Json{{"s", state.big_blind_seat}, {"amt", state.big_blind_posted}}}};

    std::array<int, kMaxSeats> received{};
    for (const Payout& payout : result.payouts) received[payout.seat] += payout.amount;
    if (result.refund) received[result.refund->seat] += result.refund->amount;

    const bool flop_seen = state.board.size() >= 3;
    const auto folded_preflop = [&state](int seat) {
        return std::any_of(state.actions.begin(), state.actions.end(), [seat](const ActionRecord& a) {
            return a.seat == seat && a.street == Street::Preflop && a.type == ActionType::Fold;
        });
    };

    Json seats = Json::array();
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state.seats[seat];
        if (!s.hole) continue;  // Not dealt in: busted on an earlier hand
        Json entry{{"s", seat},
                   {"p", context.seat_labels[seat]},
                   {"pos", std::string(to_string(*state.position_of(seat)))},
                   {"st", s.stack + s.total_bet - received[seat]}};
        if ((flop_seen && !folded_preflop(seat)) || context.hero_seat == seat) entry["c"] = cards_text(*s.hole);
        seats.push_back(std::move(entry));
    }
    root["seats"] = std::move(seats);

    for (std::size_t street = 0; street < kStreetKeys.size(); ++street) {
        const std::size_t cards_by_end = street == 0 ? 0 : street + 2;
        if (street > 0 && state.board.size() < cards_by_end) break;

        Json entry = Json::object();
        if (street > 0) {
            const std::size_t first = street == 1 ? 0 : cards_by_end - 1;
            std::string cards;
            for (std::size_t i = first; i < cards_by_end; ++i) cards += state.board[i].to_string();
            entry["b"] = std::move(cards);
        }
        entry["pot"] = state.street_start_pot[street];

        Json actions = Json::array();
        for (const ActionRecord& a : state.actions) {
            if (static_cast<std::size_t>(a.street) != street) continue;
            actions.push_back(std::string(to_string(*state.position_of(a.seat))) + ":" +
                              action_code(a.type, a.all_in, a.chips_added, a.street_total));
        }
        entry["a"] = std::move(actions);
        root[kStreetKeys[street]] = std::move(entry);
    }

    Json win = Json::array();
    for (const Payout& payout : result.payouts) {
        Json entry{{"p", context.seat_labels[payout.seat]}, {"amt", payout.amount}};
        if (result.showdown_ranks[payout.seat]) entry["hand"] = describe_hand(*result.showdown_ranks[payout.seat]);
        win.push_back(std::move(entry));
    }
    root["win"] = std::move(win);

    if (result.refund) root["ref"] = Json{{"s", result.refund->seat}, {"amt", result.refund->amount}};

    Json mistakes = Json::array();
    for (const HeroMistake& mistake : context.hero_mistakes) mistakes.push_back(mistake_json(mistake));
    root["hero_mistakes"] = std::move(mistakes);

    return to_line(root);
}

HandLogger::HandLogger(const std::filesystem::path& path) : out_(open_for_append(path)) {}

std::optional<HeroMistake> HandLogger::record_hero_decision(const PlayerView& view, const HintRecommendation& hint,
                                                            const Action& taken) {
    std::optional<HeroMistake> mistake = assess_hero_decision(view, hint, taken);
    if (mistake) pending_mistakes_.push_back(*mistake);
    return mistake;
}

std::vector<HeroMistake> HandLogger::write_hand(const GameState& state, HandLogContext context) {
    context.hero_mistakes = pending_mistakes_;
    write_line(out_, serialize_hand(state, context));
    return std::exchange(pending_mistakes_, {});
}

SessionLogger::SessionLogger(const std::array<int, kMaxSeats>& starting_stacks, const std::filesystem::path& path)
    : starting_stacks_(starting_stacks), out_(open_for_append(path)) {}

void SessionLogger::record_hand_mistakes(const std::vector<HeroMistake>& mistakes) {
    for (const HeroMistake& mistake : mistakes) {
        ++mistakes_;
        ev_loss_ += mistake.ev_loss;
        ++mistakes_by_street_[static_cast<std::size_t>(mistake.street)];
    }
}

void SessionLogger::write_session(const SessionSummary& summary, const HandLogContext& context,
                                  std::optional<std::string> timestamp) {
    if (!timestamp) {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        timestamp = std::format("{:%FT%TZ}", now);
    }

    Json root;
    root["session_id"] = context.session_id;
    root["ts"] = *timestamp;
    root["sb"] = context.small_blind;
    root["bb"] = context.big_blind;
    root["hands"] = summary.hands_played;
    root["done"] = summary.hero_busted || summary.winner.has_value();
    if (summary.winner) root["winner"] = context.seat_labels[*summary.winner];

    Json seats = Json::array();
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (starting_stacks_[seat] == 0) continue;  // Never played
        Json entry{{"s", seat},
                   {"p", context.seat_labels[seat]},
                   {"st", starting_stacks_[seat]},
                   {"end", summary.final_stacks[seat]}};
        if (summary.busted_on_hand[seat]) entry["bust"] = *summary.busted_on_hand[seat];
        seats.push_back(std::move(entry));
    }
    root["seats"] = std::move(seats);

    if (context.hero_seat) {
        const int hero = *context.hero_seat;
        Json by_street = Json::object();
        for (std::size_t street = 0; street < kStreetKeys.size(); ++street) {
            by_street[kStreetKeys[street]] = mistakes_by_street_[street];
        }
        root["hero"] = Json{{"s", hero},
                            {"net", summary.final_stacks[hero] - starting_stacks_[hero]},
                            {"bust", summary.hero_busted},
                            {"mistakes", mistakes_},
                            {"ev_loss", one_decimal(ev_loss_)},
                            {"by_street", std::move(by_street)}};
    }

    write_line(out_, to_line(root));
}

}  // namespace poker
