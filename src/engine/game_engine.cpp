#include "engine/game_engine.h"

#include <algorithm>
#include <bitset>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/deck.h"
#include "engine/pots.h"

namespace poker {
namespace {

std::uint8_t bit(ActionType type) {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(type));
}

void validate_level(const BlindLevel& level) {
    if (level.small_blind <= 0 || level.big_blind < level.small_blind) {
        throw std::invalid_argument("BlindLevel: need 0 < small_blind <= big_blind");
    }
    if (level.ante < 0) throw std::invalid_argument("BlindLevel: negative ante");
}

BlindLevel level_of(const GameConfig& config) {
    return BlindLevel{config.small_blind, config.big_blind, config.ante, config.ante_type};
}

void validate_config(const GameConfig& config) {
    validate_level(level_of(config));
    if (config.hero_seat && (*config.hero_seat < 0 || *config.hero_seat >= kMaxSeats)) {
        throw std::invalid_argument("GameConfig: hero_seat out of range");
    }
    if (config.first_button && (*config.first_button < 0 || *config.first_button >= kMaxSeats)) {
        throw std::invalid_argument("GameConfig: first_button out of range");
    }

    int live_seats = 0;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const int stack = config.stacks ? (*config.stacks)[seat] : config.starting_stack;
        if (stack < 0) throw std::invalid_argument("GameConfig: negative stack");
        if (stack > 0) ++live_seats;
    }
    if (live_seats < 2) throw std::invalid_argument("GameConfig: need at least two players with chips");

    if (config.first_button && config.stacks && (*config.stacks)[*config.first_button] == 0) {
        throw std::invalid_argument("GameConfig: first_button seat has no chips");
    }
    if (config.hero_seat && config.stacks && (*config.stacks)[*config.hero_seat] == 0) {
        throw std::invalid_argument("GameConfig: hero_seat has no chips");
    }
}

void validate_script(const ScriptedDeal& script, const GameState& state) {
    std::bitset<kDeckSize> seen;
    const auto add = [&seen](Card card) {
        if (card.id >= kDeckSize || seen.test(card.id)) {
            throw std::invalid_argument("ScriptedDeal: duplicate or invalid card");
        }
        seen.set(card.id);
    };

    for (const Card card : script.board) add(card);
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const auto& hole = script.holes[seat];
        if (state.seats[seat].status != SeatStatus::Busted && !hole) {
            throw std::invalid_argument("ScriptedDeal: live seat without hole cards");
        }
        if (hole) {
            add((*hole)[0]);
            add((*hole)[1]);
        }
    }
}

}  // namespace

std::optional<Position> GameState::position_of(int seat) const {
    if (!seats[seat].hole) return std::nullopt;

    // Dealt-in seats clockwise from the button, the button itself being first.
    std::vector<int> order;
    for (int offset = 0; offset < kMaxSeats; ++offset) {
        const int candidate = (button + offset) % kMaxSeats;
        if (seats[candidate].hole) order.push_back(candidate);
    }
    const auto index = static_cast<int>(std::find(order.begin(), order.end(), seat) - order.begin());
    const int players = static_cast<int>(order.size());

    if (index == 0) return Position::BTN;
    if (players == 2) return Position::BB;
    if (index == 1) return Position::SB;
    if (index == 2) return Position::BB;
    switch (players - index) {  // Seats before the button
        case 1: return Position::CO;
        case 2: return Position::HJ;
        default: return Position::UTG;
    }
}

GameEngine::GameEngine(GameConfig config) : config_(std::move(config)), rng_(config_.seed) {
    validate_config(config_);
    new_game();
}

void GameEngine::new_game(GameConfig config) {
    validate_config(config);
    config_ = std::move(config);
    new_game();
}

void GameEngine::set_level(const BlindLevel& level) {
    if (state_.phase != GamePhase::BetweenHands) {
        throw std::logic_error("GameEngine::set_level: not between hands");
    }
    validate_level(level);
    level_ = level;
}

void GameEngine::new_game() {
    level_ = level_of(config_);  // A level set for the last game does not carry over
    state_ = GameState{};
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        Seat& s = state_.seats[seat];
        s.stack = config_.stacks ? (*config_.stacks)[seat] : config_.starting_stack;
        s.status = s.stack > 0 ? SeatStatus::Active : SeatStatus::Busted;
    }
}

int GameEngine::next_live_seat(int from) const {
    for (int offset = 1; offset <= kMaxSeats; ++offset) {
        const int seat = (from + offset) % kMaxSeats;
        if (state_.seats[seat].status != SeatStatus::Busted) return seat;
    }
    throw std::logic_error("GameEngine: no live seats");
}

bool GameEngine::needs_action(int seat) const {
    const Seat& s = state_.seats[seat];
    return s.status == SeatStatus::Active && (!s.acted || s.street_bet < state_.current_bet);
}

int GameEngine::count_with_status(SeatStatus status) const {
    return static_cast<int>(std::count_if(state_.seats.begin(), state_.seats.end(),
                                          [status](const Seat& s) { return s.status == status; }));
}

bool GameEngine::betting_round_complete() const {
    if (count_with_status(SeatStatus::Active) <= 1) {
        // A lone player with chips has nobody to bet against, so acts only if an
        // all-in player has put in more than they have. The bet to call is not
        // enough: a short big blind leaves it above what anyone has put in.
        int highest_all_in = 0;
        for (const Seat& s : state_.seats) {
            if (s.status == SeatStatus::AllIn) highest_all_in = std::max(highest_all_in, s.street_bet);
        }
        return std::none_of(state_.seats.begin(), state_.seats.end(), [highest_all_in](const Seat& s) {
            return s.status == SeatStatus::Active && s.street_bet < highest_all_in;
        });
    }
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (needs_action(seat)) return false;
    }
    return true;
}

void GameEngine::commit_chips(int seat, int street_total) {
    Seat& s = state_.seats[seat];
    const int added = street_total - s.street_bet;
    s.stack -= added;
    s.street_bet = street_total;
    s.total_bet += added;
    if (s.stack == 0) s.status = SeatStatus::AllIn;
}

// Antes touch only the stack: they are not part of any bet.
void GameEngine::post_ante(int seat, int amount) {
    Seat& s = state_.seats[seat];
    const int paid = std::min(amount, s.stack);
    s.stack -= paid;
    s.ante_posted += paid;
    if (s.stack == 0) s.status = SeatStatus::AllIn;
}

// What each seat has put in, for splitting pots. Per-player antes count, so a
// player all in for less than the ante gets a side pot of the right size; a
// big-blind ante is dead money for the main pot instead (see main_pot_dead_money).
std::array<int, kMaxSeats> GameEngine::pot_contributions() const {
    std::array<int, kMaxSeats> contributed{};
    const bool antes_layer = state_.level.ante_type == AnteType::PerPlayer;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state_.seats[seat];
        contributed[seat] = s.total_bet + (antes_layer ? s.ante_posted : 0);
    }
    return contributed;
}

int GameEngine::main_pot_dead_money() const {
    return state_.level.ante_type == AnteType::BigBlind ? state_.total_antes() : 0;
}

void GameEngine::start_hand(const std::optional<ScriptedDeal>& script) {
    if (state_.phase != GamePhase::BetweenHands) {
        throw std::logic_error("GameEngine::start_hand: not between hands");
    }
    if (script) validate_script(*script, state_);

    for (Seat& s : state_.seats) {
        s.street_bet = 0;
        s.total_bet = 0;
        s.ante_posted = 0;
        s.hole.reset();
        s.acted = false;
        s.level_acted_at = 0;
        if (s.status != SeatStatus::Busted) s.status = SeatStatus::Active;
    }
    state_.board.clear();
    state_.actions.clear();
    state_.last_result.reset();
    state_.street_start_pot = {};
    state_.street = Street::Preflop;
    state_.phase = GamePhase::Betting;
    state_.level = level_;
    ++state_.hand_number;

    // Button.
    if (state_.button >= 0) {
        state_.button = next_live_seat(state_.button);
    } else if (config_.first_button) {
        state_.button = *config_.first_button;
    } else {
        std::vector<int> live;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (state_.seats[seat].status != SeatStatus::Busted) live.push_back(seat);
        }
        state_.button = live[static_cast<std::size_t>(rng_() % live.size())];
    }

    // Blinds seats. Heads-up, the button is the small blind.
    const bool heads_up = count_with_status(SeatStatus::Active) == 2;
    state_.small_blind_seat = heads_up ? state_.button : next_live_seat(state_.button);
    state_.big_blind_seat = next_live_seat(state_.small_blind_seat);

    // Cards. All five board cards are fixed now and revealed street by street.
    if (script) {
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (state_.seats[seat].status != SeatStatus::Busted) state_.seats[seat].hole = script->holes[seat];
        }
        planned_board_ = script->board;
    } else {
        Deck deck;
        deck.shuffle(rng_);
        const int first_dealt = next_live_seat(state_.button);
        std::array<std::array<Card, 2>, kMaxSeats> holes{};
        for (std::size_t card = 0; card < 2; ++card) {
            int seat = first_dealt;
            do {
                holes[seat][card] = deck.deal();
                seat = next_live_seat(seat);
            } while (seat != first_dealt);
        }
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (state_.seats[seat].status != SeatStatus::Busted) state_.seats[seat].hole = holes[seat];
        }
        for (Card& card : planned_board_) card = deck.deal();
    }

    // Per-player antes come first, then the blinds, each capped at the poster's
    // stack; a big-blind ante comes from what the blind leaves. The blind seats
    // were chosen above, so a seat all in from its ante still posts (nothing).
    const BlindLevel& level = state_.level;
    if (level.has_ante() && level.ante_type == AnteType::PerPlayer) {
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (state_.seats[seat].status != SeatStatus::Busted) post_ante(seat, level.ante);
        }
    }
    state_.small_blind_posted = std::min(level.small_blind, state_.seats[state_.small_blind_seat].stack);
    state_.big_blind_posted = std::min(level.big_blind, state_.seats[state_.big_blind_seat].stack);
    commit_chips(state_.small_blind_seat, state_.small_blind_posted);
    commit_chips(state_.big_blind_seat, state_.big_blind_posted);
    if (level.has_ante() && level.ante_type == AnteType::BigBlind) post_ante(state_.big_blind_seat, level.ante);
    state_.current_bet = level.big_blind;
    state_.last_raise_size = level.big_blind;
    state_.street_start_pot[static_cast<std::size_t>(Street::Preflop)] = state_.total_pot();

    settle(state_.big_blind_seat);
}

LivePots GameEngine::live_pots() const {
    std::array<SeatStatus, kMaxSeats> statuses{};
    for (int seat = 0; seat < kMaxSeats; ++seat) statuses[seat] = state_.seats[seat].status;
    return calculate_live_pots(pot_contributions(), statuses, main_pot_dead_money());
}

LegalActions GameEngine::legal_actions() const {
    LegalActions legal;
    if (!state_.to_act) return legal;

    const int seat = *state_.to_act;
    const Seat& s = state_.seats[seat];
    const int to_call = std::max(0, state_.current_bet - s.street_bet);

    bool opponent_can_act = false;
    for (int other = 0; other < kMaxSeats; ++other) {
        if (other != seat && state_.seats[other].status == SeatStatus::Active) opponent_can_act = true;
    }
    const bool betting_open = !s.acted || state_.current_bet - s.level_acted_at >= state_.last_raise_size;
    const bool can_aggress = s.stack > to_call && opponent_can_act && betting_open;

    legal.mask = bit(ActionType::Fold);
    if (to_call == 0) {
        legal.mask |= bit(ActionType::Check);
    } else {
        legal.mask |= bit(ActionType::Call);
        legal.call_amount = std::min(to_call, s.stack);
    }
    if (can_aggress) {
        legal.mask |= bit(state_.current_bet == 0 ? ActionType::Bet : ActionType::Raise);
        legal.max_raise_to = s.street_bet + s.stack;
        legal.min_raise_to = std::min(state_.current_bet + state_.last_raise_size, legal.max_raise_to);
    }
    if (can_aggress || (to_call > 0 && s.stack <= to_call)) {
        legal.mask |= bit(ActionType::AllIn);
    }
    return legal;
}

PlayerView GameEngine::player_view(int seat) const {
    return make_player_view(state_, seat, legal_actions());
}

bool GameEngine::apply_action(const Action& action) {
    const LegalActions legal = legal_actions();
    if (!legal.allows(action.type)) return false;

    const int seat = *state_.to_act;
    Seat& s = state_.seats[seat];
    const int before = s.street_bet;

    ActionType resolved = action.type;
    int street_total = before;
    switch (action.type) {
        case ActionType::Fold:
            s.status = SeatStatus::Folded;
            break;
        case ActionType::Check:
            break;
        case ActionType::Call:
            street_total = before + legal.call_amount;
            break;
        case ActionType::Bet:
        case ActionType::Raise:
            if (action.amount < legal.min_raise_to || action.amount > legal.max_raise_to) return false;
            street_total = action.amount;
            break;
        case ActionType::AllIn:
            street_total = before + s.stack;
            if (street_total <= state_.current_bet) {
                resolved = ActionType::Call;
            } else {
                resolved = state_.current_bet == 0 ? ActionType::Bet : ActionType::Raise;
            }
            break;
    }

    if (street_total != before) commit_chips(seat, street_total);

    // Only a full bet or raise sets the minimum for the next raise.
    if (street_total > state_.current_bet) {
        state_.last_raise_size = std::max(state_.last_raise_size, street_total - state_.current_bet);
        state_.current_bet = street_total;
    }
    s.acted = true;
    s.level_acted_at = state_.current_bet;

    state_.actions.push_back(ActionRecord{state_.street, seat, resolved, street_total - before, street_total,
                                          s.status == SeatStatus::AllIn});

    if (count_with_status(SeatStatus::Folded) + count_with_status(SeatStatus::Busted) == kMaxSeats - 1) {
        finish_hand(false);
    } else {
        settle(seat);
    }
    return true;
}

// Finds the next player to act after `from_seat`, moving through streets as
// betting rounds close and ending the hand when the river round is done.
void GameEngine::settle(int from_seat) {
    while (betting_round_complete()) {
        for (Seat& s : state_.seats) {
            s.street_bet = 0;
            s.acted = false;
            s.level_acted_at = 0;
        }
        state_.current_bet = 0;
        state_.last_raise_size = state_.level.big_blind;
        state_.street = static_cast<Street>(static_cast<int>(state_.street) + 1);

        if (state_.street == Street::Showdown) {
            finish_hand(true);
            return;
        }
        deal_street_cards();
        state_.street_start_pot[static_cast<std::size_t>(state_.street)] = state_.total_pot();
        from_seat = state_.button;  // Postflop action starts left of the button
    }

    for (int offset = 1; offset <= kMaxSeats; ++offset) {
        const int seat = (from_seat + offset) % kMaxSeats;
        if (needs_action(seat)) {
            state_.to_act = seat;
            return;
        }
    }
    throw std::logic_error("GameEngine::settle: open betting round with nobody to act");
}

void GameEngine::deal_street_cards() {
    const std::size_t target = state_.street == Street::Flop ? 3 : state_.board.size() + 1;
    while (state_.board.size() < target) state_.board.push_back(planned_board_[state_.board.size()]);
}

void GameEngine::finish_hand(bool showdown) {
    std::array<bool, kMaxSeats> live{};
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state_.seats[seat];
        live[seat] = s.status == SeatStatus::Active || s.status == SeatStatus::AllIn;
    }

    PotBreakdown breakdown = calculate_pots(pot_contributions(), live, main_pot_dead_money());

    HandResult result;
    result.showdown = showdown;
    if (showdown) {
        std::array<Card, 5> board{};
        std::copy(state_.board.begin(), state_.board.end(), board.begin());
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (live[seat]) result.showdown_ranks[seat] = evaluate7(*state_.seats[seat].hole, board);
        }
    }

    std::array<int, kMaxSeats> won{};
    for (const Pot& pot : breakdown.pots) {
        // Winners in clockwise order from the button; odd chips go to the earliest.
        std::vector<int> winners;
        HandRank best = kWorstHandRank + 1;
        for (int offset = 1; offset <= kMaxSeats; ++offset) {
            const int seat = (state_.button + offset) % kMaxSeats;
            if (std::find(pot.eligible.begin(), pot.eligible.end(), seat) == pot.eligible.end()) continue;

            const HandRank rank = showdown ? *result.showdown_ranks[seat] : kBestHandRank;
            if (rank < best) {
                best = rank;
                winners.clear();
            }
            if (rank == best) winners.push_back(seat);
        }

        const int count = static_cast<int>(winners.size());
        for (int i = 0; i < count; ++i) {
            won[winners[i]] += pot.amount / count + (i < pot.amount % count ? 1 : 0);
        }
    }

    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (won[seat] > 0) {
            result.payouts.push_back(Payout{seat, won[seat]});
            state_.seats[seat].stack += won[seat];
        }
    }
    if (breakdown.refund) state_.seats[breakdown.refund->seat].stack += breakdown.refund->amount;
    result.pots = std::move(breakdown.pots);
    result.refund = breakdown.refund;
    state_.last_result = std::move(result);

    // Freezeout: no chips means out for good.
    for (Seat& s : state_.seats) {
        if (s.status != SeatStatus::Busted && s.stack == 0) {
            s.status = SeatStatus::Busted;
            s.busted_on_hand = state_.hand_number;
        }
    }

    state_.to_act.reset();
    const int players_left = kMaxSeats - count_with_status(SeatStatus::Busted);
    const bool hero_busted = config_.hero_seat && state_.seats[*config_.hero_seat].status == SeatStatus::Busted;
    state_.phase = (players_left <= 1 || hero_busted) ? GamePhase::SessionOver : GamePhase::BetweenHands;
}

SessionSummary GameEngine::session_summary() const {
    SessionSummary summary;
    summary.hands_played = state_.hand_number;
    int players_left = 0;
    int last_live = -1;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        const Seat& s = state_.seats[seat];
        summary.final_stacks[seat] = s.stack + (s.total_bet + s.ante_posted) * (state_.phase == GamePhase::Betting ? 1 : 0);
        summary.busted_on_hand[seat] = s.busted_on_hand;
        if (s.status != SeatStatus::Busted) {
            ++players_left;
            last_live = seat;
        }
    }
    if (players_left == 1) summary.winner = last_live;
    summary.hero_busted = config_.hero_seat && state_.seats[*config_.hero_seat].status == SeatStatus::Busted;
    return summary;
}

}  // namespace poker
