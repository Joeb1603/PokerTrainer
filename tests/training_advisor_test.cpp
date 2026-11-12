#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "advisor/advisor_charts.h"
#include "advisor/training_advisor.h"
#include "bots/gto_heuristic_engine.h"
#include "controller/game_controller.h"
#include "engine/game_engine.h"
#include "range/hand_vs_range.h"
#include "range/range_model.h"
#include "strategy/preflop_charts.h"
#include "strategy/push_fold.h"

using namespace poker;

namespace {

static_assert(std::is_invocable_v<decltype(&TrainingAdvisor::analyze), const TrainingAdvisor&, const PlayerView&>);
static_assert(!std::is_invocable_v<decltype(&TrainingAdvisor::analyze), const TrainingAdvisor&, const GameState&>);

constexpr int kRollouts = 2000;
// For loops over many states: few rollouts, a sampled flop and one thread.
constexpr AdvisorOptions kFastOptions{100, 500, 1, 1, false};

Card card(const char* text) { return Card::parse(text).value(); }
std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }
std::array<Card, 5> board_of(const char* a, const char* b, const char* c, const char* d, const char* e) {
    return {card(a), card(b), card(c), card(d), card(e)};
}

GameEngine heads_up_to_flop(const std::array<Card, 2>& seat0, const std::array<Card, 2>& seat1,
                            const std::array<Card, 5>& board) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = seat0;
    deal.holes[1] = seat1;
    deal.board = board;
    engine.start_hand(deal);
    EXPECT_TRUE(engine.apply_action({ActionType::Raise, 100}));
    EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    return engine;
}

GameEngine six_max(int seat, const std::array<Card, 2>& cards, std::uint64_t seed = 5, int stack = 2000,
                   AnteType ante_type = AnteType::None, int ante = 0) {
    GameConfig config;
    config.ante_type = ante_type;
    config.ante = ante;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.seed = seed;
    config.stacks = std::array<int, kMaxSeats>{stack, stack, stack, stack, stack, stack};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[seat] = cards;
    deal.board = board_of("2c", "7d", "Jh", "4s", "9c");
    std::array<bool, kDeckSize> used{};
    for (const Card c : deal.board) used[c.id] = true;
    used[cards[0].id] = true;
    used[cards[1].id] = true;
    int next = 0;
    const auto spare = [&] {
        while (used[static_cast<std::size_t>(next)]) ++next;
        used[static_cast<std::size_t>(next)] = true;
        return Card(next);
    };
    for (int s = 0; s < kMaxSeats; ++s) {
        if (!deal.holes[s]) deal.holes[s] = std::array<Card, 2>{spare(), spare()};
    }
    engine.start_hand(deal);
    return engine;
}

// BTN to act behind three limpers.
GameEngine six_max_limped(const std::array<Card, 2>& cards) {
    GameEngine engine = six_max(0, cards);
    for (int limp = 0; limp < 3; ++limp) EXPECT_TRUE(engine.apply_action({ActionType::Call, 0}));
    return engine;
}

PreflopResponse chart_response_for_shove(const char* text) {
    const HandClass hand = HandClass::parse(text).value();
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            if (HandClass::from_cards(Card(a), Card(b)) != hand || a == card("2c").id || b == card("2c").id ||
                a == card("7d").id || b == card("7d").id || a == card("Jh").id || b == card("Jh").id ||
                a == card("4s").id || b == card("4s").id || a == card("9c").id || b == card("9c").id) {
                continue;
            }
            GameEngine engine = six_max(2, {Card(a), Card(b)});
            while (engine.state().to_act != 3) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
            while (engine.state().to_act != 2) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            return advisor_chart_response(engine.player_view(2));
        }
    }
    throw std::logic_error("no cards for hand");
}

bool is_aggressive(ActionType type) {
    return type == ActionType::Bet || type == ActionType::Raise || type == ActionType::AllIn;
}

bool is_legal(const LegalActions& legal, const Action& a) {
    if (!legal.allows(a.type)) return false;
    if (a.type == ActionType::Bet || a.type == ActionType::Raise) {
        return a.amount >= legal.min_raise_to && a.amount <= legal.max_raise_to;
    }
    return true;
}

double best_listed_ev(const EvEstimates& ev) {
    double best = -1e18;
    for (const auto& option : {ev.fold, ev.check_call, ev.raise_25, ev.raise_50, ev.raise_75, ev.raise_100, ev.all_in}) {
        if (option) best = std::max(best, *option);
    }
    return best;
}

double ev_of_action(const HintRecommendation& hint) {
    switch (hint.action.type) {
        case ActionType::Fold: return hint.ev.fold.value();
        case ActionType::Check:
        case ActionType::Call: return hint.ev.check_call.value();
        case ActionType::AllIn: return hint.ev.all_in.value();
        case ActionType::Bet:
        case ActionType::Raise:
            if (hint.raise_25_to == hint.action.amount) return hint.ev.raise_25.value();
            if (hint.raise_50_to == hint.action.amount) return hint.ev.raise_50.value();
            if (hint.raise_75_to == hint.action.amount) return hint.ev.raise_75.value();
            return hint.ev.raise_100.value();
    }
    return 0.0;
}

// The EV a line lists for its own advice; its sizes are the hint's ladder.
double line_ev_of_action(const HintRecommendation& hint, const AdviceLine& line) {
    switch (line.action.type) {
        case ActionType::Fold: return line.ev.fold.value();
        case ActionType::Check:
        case ActionType::Call: return line.ev.check_call.value();
        case ActionType::AllIn: return line.ev.all_in.value();
        case ActionType::Bet:
        case ActionType::Raise:
            if (hint.raise_25_to == line.action.amount) return line.ev.raise_25.value();
            if (hint.raise_50_to == line.action.amount) return line.ev.raise_50.value();
            if (hint.raise_75_to == line.action.amount) return line.ev.raise_75.value();
            if (hint.raise_100_to == line.action.amount) return line.ev.raise_100.value();
            ADD_FAILURE() << "line bet off the ladder: " << line.best_action;
    }
    return 0.0;
}

// The archetype lines, when present: all three, in order, each legal and the
// highest option it lists, with outcomes for exactly Baseline's raise rows.
void expect_consistent_lines(const PlayerView& view, const HintRecommendation& hint) {
    const bool present = hint.versus[0].has_value();
    constexpr std::array<Archetype, 3> kOrder{Archetype::Nit, Archetype::Station, Archetype::Maniac};
    for (std::size_t i = 0; i < hint.versus.size(); ++i) {
        ASSERT_EQ(hint.versus[i].has_value(), present) << i;
        if (!present) continue;
        const AdviceLine& line = *hint.versus[i];
        EXPECT_EQ(line.versus, kOrder[i]);
        EXPECT_TRUE(is_legal(view.legal, line.action)) << line.best_action;
        EXPECT_FALSE(line.best_action.empty());
        EXPECT_DOUBLE_EQ(line_ev_of_action(hint, line), best_listed_ev(line.ev)) << line.best_action;
        EXPECT_EQ(line.ev.fold.has_value(), hint.ev.fold.has_value());
        EXPECT_EQ(line.ev.check_call.has_value(), hint.ev.check_call.has_value());
        EXPECT_EQ(line.ev.all_in.has_value(), hint.ev.all_in.has_value());
        EXPECT_EQ(line.ev.raise_25.has_value(), hint.ev.raise_25.has_value());
        EXPECT_EQ(line.ev.raise_100.has_value(), hint.ev.raise_100.has_value());
        for (std::size_t o = 0; o < kOutcomeCount; ++o) {
            ASSERT_EQ(line.outcomes[o].has_value(), hint.outcomes[o].has_value()) << o;
            if (!line.outcomes[o]) continue;
            EXPECT_GE(line.outcomes[o]->fold_pct, 0.0);
            EXPECT_GE(line.outcomes[o]->raise_pct, 0.0);
            EXPECT_LE(line.outcomes[o]->fold_pct + line.outcomes[o]->raise_pct, 100.0 + 1e-9);
            EXPECT_GE(line.outcomes[o]->equity_called_pct, 0.0);
            EXPECT_LE(line.outcomes[o]->equity_called_pct, 100.0 + 1e-9);
        }
        EXPECT_GE(line.range_equity_pct, 0.0);
        EXPECT_LE(line.range_equity_pct, 100.0 + 1e-9);
        EXPECT_LE(line.why.size(), 140u) << line.why;
        const bool same = line.action.type == hint.action.type && line.action.amount == hint.action.amount;
        const std::string lead = same ? "Same as Baseline: " : "vs " + std::string(to_string(line.versus)) + ": ";
        EXPECT_EQ(line.why.rfind(lead, 0), 0u) << line.why;
    }
}

// The predicted answers are listed for exactly the raise rows listed, and are shares.
void expect_consistent_outcomes(const PlayerView& view, const HintRecommendation& hint) {
    const std::array<std::optional<double>, 4> rows{hint.base_ev.raise_25, hint.base_ev.raise_50, hint.base_ev.raise_75,
                                                    hint.base_ev.raise_100};
    const bool may_raise = view.legal.allows(ActionType::Bet) || view.legal.allows(ActionType::Raise);
    for (std::size_t i = 0; i < rows.size(); ++i) EXPECT_EQ(hint.outcomes[i].has_value(), rows[i].has_value()) << i;
    EXPECT_EQ(hint.outcomes[kOutcomeAllIn].has_value(), hint.base_ev.all_in.has_value() && may_raise);
    for (const std::optional<BetOutcome>& o : hint.outcomes) {
        if (!o) continue;
        EXPECT_GE(o->fold_pct, 0.0);
        EXPECT_GE(o->raise_pct, 0.0);
        EXPECT_LE(o->fold_pct + o->raise_pct, 100.0 + 1e-9);
        for (const double e : {o->equity_called_pct, o->equity_raised_pct}) {
            EXPECT_GE(e, 0.0);
            EXPECT_LE(e, 100.0 + 1e-9);
        }
    }
    EXPECT_GE(hint.range_equity_pct, 0.0);
    EXPECT_LE(hint.range_equity_pct, 100.0 + 1e-9);
    expect_consistent_lines(view, hint);
}

TEST(Advisor, PotOddsAndMdfAreExactInAKnownSpot) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));  // 150 into 200
    const HintRecommendation hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(0));
    EXPECT_DOUBLE_EQ(hint.pot_odds_pct, 100.0 * 150.0 / 500.0);
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0 * 200.0 / 350.0);
    EXPECT_DOUBLE_EQ(hint.spr, 1750.0 / 500.0);  // Each has 1750 behind and 500 is in the pot after calling
}

TEST(Advisor, NothingToCallMeansNoOddsAndFullDefense) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(1));
    EXPECT_DOUBLE_EQ(hint.pot_odds_pct, 0.0);
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0);
    EXPECT_DOUBLE_EQ(hint.spr, 1900.0 / 200.0);
}

TEST(Advisor, EvFormulasMatchTheDocumentedModelForCheckAndCall) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation check_hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(1));
    EXPECT_NEAR(*check_hint.ev.check_call, check_hint.range_equity_pct / 100.0 * 200.0, 1e-9);

    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));
    const HintRecommendation call_hint = TrainingAdvisor(1, kRollouts).analyze(engine.player_view(0));
    EXPECT_EQ(*call_hint.ev.fold, 0.0);
    // Heads-up, so the layered equity is the range equity, with no pressure discount.
    EXPECT_NEAR(*call_hint.ev.check_call, call_hint.range_equity_pct / 100.0 * 500.0 - 150.0, 1e-9);
}

// The documented bet formula, by hand, heads-up with nobody's chips in yet this
// street: F * P + P_call * (e_c * (P + 2a) - a + implied) + P_raise * V_raised. The
// raise is pot-sized, to R2 = a + (P + 2a) capped at the stack, which Hero calls for
// R2 into a final pot of P + 2 R2 (or folds for -a). The implied term is capped at
// half the pot once called, and is zero on the river.
double unfaced_bet_ev(const BetOutcome& o, double pot, double added, double stack, bool river) {
    const double fold = o.fold_pct / 100.0;
    const double raise = o.raise_pct / 100.0;
    const double call = 1.0 - fold - raise;
    const double e_call = o.equity_called_pct / 100.0;
    const double e_raise = o.equity_raised_pct / 100.0;
    const double implied =
        river ? 0.0 : std::min(std::max(0.0, e_call - 0.5) * (stack - added) * 0.5, 0.5 * (pot + added + added));
    const double reraise_to = std::min(added + pot + 2.0 * added, stack);
    const double raised = std::max(-added, e_raise * (pot + 2.0 * reraise_to) - reraise_to);
    return fold * pot + call * (e_call * (pot + 2.0 * added) - added + implied) + raise * raised;
}

TEST(Advisor, RaiseEvsMatchTheDocumentedModel) {
    // A set on a rainbow flop, checked to. Pot 200, 1900 behind each.
    GameEngine engine = heads_up_to_flop(hole("Ac", "Ah"), hole("8c", "8d"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(20, kRollouts).analyze(engine.player_view(1));
    ASSERT_EQ(hint.raise_25_to, 50);
    ASSERT_EQ(hint.raise_50_to, 100);
    ASSERT_EQ(hint.raise_75_to, 150);
    ASSERT_EQ(hint.raise_100_to, 200);
    const std::array<std::pair<std::optional<double>, double>, 4> rows{{{hint.base_ev.raise_25, 50.0},
                                                                         {hint.base_ev.raise_50, 100.0},
                                                                         {hint.base_ev.raise_75, 150.0},
                                                                         {hint.base_ev.raise_100, 200.0}}};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ASSERT_TRUE(hint.outcomes[i].has_value()) << i;
        EXPECT_NEAR(*rows[i].first, unfaced_bet_ev(*hint.outcomes[i], 200.0, rows[i].second, 1900.0, false), 1e-9);
        EXPECT_GT(hint.outcomes[i]->fold_pct, 0.0);  // Some of the range always folds
        EXPECT_GT(hint.outcomes[i]->equity_called_pct, 50.0);  // A set is ahead of what calls
    }
    ASSERT_TRUE(hint.outcomes[kOutcomeAllIn].has_value());
    EXPECT_NEAR(*hint.base_ev.all_in, unfaced_bet_ev(*hint.outcomes[kOutcomeAllIn], 200.0, 1900.0, 1900.0, false), 1e-9);
    // Bigger bets fold out more of the range.
    EXPECT_LT(hint.outcomes[0]->fold_pct, hint.outcomes[3]->fold_pct);
    EXPECT_LT(hint.outcomes[3]->fold_pct, hint.outcomes[kOutcomeAllIn]->fold_pct);
}

TEST(Advisor, RiverNutsRaiseEvsAreExact) {
    // Equity 1 against every hand and no implied odds leave only the fold share and the size.
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    const HintRecommendation hint = TrainingAdvisor(21, kRollouts).analyze(engine.player_view(1));
    ASSERT_DOUBLE_EQ(hint.equity_pct, 100.0);
    ASSERT_DOUBLE_EQ(hint.range_equity_pct, 100.0);
    ASSERT_TRUE(hint.outcomes[kOutcomeAllIn].has_value());
    ASSERT_TRUE(hint.outcomes[kOutcome75].has_value());
    for (const std::size_t i : {kOutcome75, kOutcomeAllIn}) {
        EXPECT_DOUBLE_EQ(hint.outcomes[i]->equity_called_pct, 100.0);
        if (hint.outcomes[i]->raise_pct > 0.0) { EXPECT_DOUBLE_EQ(hint.outcomes[i]->equity_raised_pct, 100.0); }
    }
    // All-in: whether called or "raised" (only a call is left), Hero wins 200 + 1900.
    const double shove_fold = hint.outcomes[kOutcomeAllIn]->fold_pct / 100.0;
    EXPECT_NEAR(*hint.base_ev.all_in, shove_fold * 200.0 + (1.0 - shove_fold) * (200.0 + 1900.0), 1e-9);
    // 150 into 200: a call wins 200 + 150. A pot-sized raise adds the 500 in the pot
    // after calling, to 650, and Hero's call of it wins 200 + 650.
    const BetOutcome& o = *hint.outcomes[kOutcome75];
    const double fold = o.fold_pct / 100.0;
    const double raise = o.raise_pct / 100.0;
    EXPECT_NEAR(*hint.base_ev.raise_75, fold * 200.0 + (1.0 - fold - raise) * 350.0 + raise * 850.0, 1e-9);
    EXPECT_NEAR(*hint.base_ev.raise_75, unfaced_bet_ev(o, 200.0, 150.0, 1900.0, true), 1e-9);
}

TEST(Advisor, DifferentDecisionsGetDifferentRollouts) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const PlayerView view = engine.player_view(3);
    const TrainingAdvisor advisor(22, kRollouts);
    const double base = advisor.analyze(view).equity_pct;

    PlayerView next_hand = view;
    ++next_hand.hand_number;
    EXPECT_NE(advisor.analyze(next_hand).equity_pct, base);

    PlayerView later_in_hand = view;
    later_in_hand.actions.push_back(ActionRecord{Street::Preflop, 4, ActionType::Fold, 0, 0, false});
    EXPECT_NE(advisor.analyze(later_in_hand).equity_pct, base);
}

// All chips above the short all-in are heads-up, so only that much is three-way.
TEST(Advisor, CallEvCountsEachPotAgainstItsOwnOpponents) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 40, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    const std::array<Card, 2> mine = hole("Qc", "5d");
    const std::array<Card, 5> board = board_of("Ks", "7c", "2d", "9h", "4s");
    deal.holes[0] = mine;
    deal.holes[1] = hole("Ac", "Ad");
    deal.holes[2] = hole("Kc", "Kd");
    deal.board = board;
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));  // The button opens
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));  // The big blind calls for 40, all in
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 1000}));
    const PlayerView view = engine.player_view(0);

    // Each layer against its own opponents' estimated ranges, by a long independent run.
    const std::vector<PotLayer> layers = pot_layers(view);
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].opponents, 2);
    EXPECT_EQ(layers[1].opponents, 1);
    std::vector<LayerRequest> requests;
    for (const PotLayer& layer : layers) requests.push_back(LayerRequest{layer.opponent_seats});
    RangeEquityOptions options;
    options.seed = 1;
    options.multiway_rollouts = 200000;
    const MultiwayRangeEquity equity = equity_vs_ranges(mine, view.board, infer_ranges(view, Archetype::Baseline),
                                                        requests, options);
    double expected = -1000.0;
    for (std::size_t i = 0; i < layers.size(); ++i) expected += layers[i].amount * equity.equity[i];
    EXPECT_GT(equity.equity[1], equity.equity[0]);  // The heads-up part is the better one

    const HintRecommendation hint = TrainingAdvisor(23, kRollouts).analyze(view);
    EXPECT_NEAR(*hint.ev.check_call, expected, 5.0);  // Monte Carlo on the 120-chip layer only
    EXPECT_EQ(hint.action.type, ActionType::Fold);
}

// The documented postflop bet/raise EV, rebuilt per villain from the range model's
// public functions, with stacks that are not all equal (training_advisor.h).
double rebuilt_raise_ev(const PlayerView& view, int to) {
    const ArchetypeParams& params = archetype_params(Archetype::Baseline);
    const PublicSeat& me = view.seats[static_cast<std::size_t>(view.seat)];
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges = infer_ranges(view, params);
    const CardMask dead = card_mask(std::span<const Card>(view.board)) | card_mask(std::span<const Card>(*view.own_hole));
    for (std::optional<WeightedRange>& range : ranges) {
        if (range) range->remove_cards(dead);
    }
    std::bitset<kComboCount> every;
    every.set();
    const ComboEquities combos = hero_vs_each_combo(*view.own_hole, view.board, every);
    const WeightedRange hero_range = infer_range_for(view, view.seat, params, false);
    const StreetStrength strength = street_strength(view.board);

    const int eff = clamp_raise_to(view, to);
    const double a = eff - me.street_bet;
    const double pot = view.total_pot;
    const double facing = std::max(0, view.current_bet - me.street_bet);
    const double size = (eff - view.current_bet) / (pot + facing);
    const int hero_total = me.street_bet + me.stack;

    // Everybody folds: Hero pays the call and plays each layer against its all-in players.
    const PotMath math = pot_math(view);
    double fold_win = -math.to_call;
    for (const PotLayer& layer : pot_layers(view)) {
        double e = 1.0;
        for (int i = 0; i < kMaxSeats; ++i) {
            if (((layer.opponent_seats >> i) & 1u) != 0 && view.seats[static_cast<std::size_t>(i)].status == SeatStatus::AllIn) {
                e *= equity_vs_range(combos, *ranges[static_cast<std::size_t>(i)]).value();  // One all-in player here
            }
        }
        fold_win += layer.amount * e;
    }

    double fold = 1.0, no_raise = 1.0, w = 0.0, we = 0.0, wm = 0.0, refund = 0.0, u = 0.0, raised = 0.0;
    int deepest = view.current_bet;
    for (int i = 0; i < kMaxSeats; ++i) {
        const PublicSeat& s = view.seats[static_cast<std::size_t>(i)];
        if (i == view.seat || s.status != SeatStatus::Active) continue;
        const int total = s.street_bet + s.stack;
        deepest = std::max(deepest, total);
        const double match = std::max(0, std::min(eff, total) - s.street_bet);
        const Response r = predict_postflop_response(*ranges[static_cast<std::size_t>(i)], strength, view.street, size,
                                                     match / (pot + a + match), params, &hero_range);
        fold *= 1.0 - r.call - r.raise;
        no_raise *= 1.0 - r.raise;
        const double ec = equity_vs_range(combos, r.call_range).value_or(0.0);
        w += r.call;
        we += r.call * ec;
        wm += r.call * match;
        refund += r.call * std::max(0, eff - total) * (1.0 - ec);  // What a short caller cannot match comes back
        const double er = equity_vs_range(combos, r.raise_range).value_or(0.0);
        const double r2 = std::min(std::max(static_cast<double>(eff), std::min({eff + pot + a + match,
                                                                                 static_cast<double>(hero_total),
                                                                                 static_cast<double>(total)})),
                                   static_cast<double>(total));  // A short "raiser" only calls all in
        u += r.raise;
        raised += r.raise * std::max(-a, er * (pot + (r2 - me.street_bet) + (r2 - s.street_bet)) - (r2 - me.street_bet));
    }
    const double p_raise = 1.0 - no_raise;
    const double p_call = 1.0 - fold - p_raise;
    const double e_call = we / w;
    const double m = wm / w;
    double implied = 0.0;
    if (view.street != Street::River) {
        implied = std::min(std::max(0.0, e_call - 0.5) * std::max(0, std::min(hero_total, deepest) - eff) * 0.5,
                           0.5 * (pot + a + m));
    }
    return fold * fold_win + p_call * (e_call * (pot + a + m) - a + refund / w + implied) +
           (u > 0.0 ? p_raise * raised / u : 0.0);
}

// Three-way with a short stack: what folds leave beside a player already all in,
// and what a villain too short to cover a shove can win from it.
TEST(Advisor, RaiseEvsCountWhatShortStacksCanMatch) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 300, 0, 0, 0};

    // The big blind is all in for 300; the small blind bets 500 into 900 on the
    // turn. If it folds to a raise, Hero pays the 500 call, plays the 900 main pot
    // against the all-in range and gets the rest back.
    {
        GameEngine engine(config);
        ScriptedDeal deal;
        deal.holes[0] = hole("Jh", "Th");
        deal.holes[1] = hole("Ac", "Kc");
        deal.holes[2] = hole("5s", "5d");
        deal.board = board_of("9h", "8c", "2h", "Kd", "3s");
        engine.start_hand(deal);
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Bet, 500}));
        const PlayerView view = engine.player_view(0);
        const HintRecommendation hint = TrainingAdvisor(24, kRollouts).analyze(view);
        ASSERT_TRUE(hint.raise_25_to.has_value());
        EXPECT_NEAR(*hint.base_ev.raise_25, rebuilt_raise_ev(view, *hint.raise_25_to), 1e-6);
        ASSERT_TRUE(hint.base_ev.all_in.has_value());
        EXPECT_NEAR(*hint.base_ev.all_in, rebuilt_raise_ev(view, view.legal.max_raise_to), 1e-6);
    }

    // Three-way flop, checked to the button, with the big blind 240 behind: a shove
    // risks only 240 against it, called or "raised".
    {
        GameEngine engine(config);
        ScriptedDeal deal;
        deal.holes[0] = hole("Ks", "Kd");
        deal.holes[1] = hole("Ac", "Qc");
        deal.holes[2] = hole("5s", "5d");
        deal.board = board_of("Kh", "8c", "2h", "Jd", "3s");
        engine.start_hand(deal);
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        const PlayerView view = engine.player_view(0);
        const HintRecommendation hint = TrainingAdvisor(25, kRollouts).analyze(view);
        ASSERT_TRUE(hint.base_ev.all_in.has_value());
        EXPECT_NEAR(*hint.base_ev.all_in, rebuilt_raise_ev(view, view.legal.max_raise_to), 1e-6);
        ASSERT_TRUE(hint.raise_100_to.has_value());
        EXPECT_NEAR(*hint.base_ev.raise_100, rebuilt_raise_ev(view, *hint.raise_100_to), 1e-6);
    }
}

// A hand in every class, avoiding six_max's fixed board.
std::array<Card, 2> cards_for(HandClass hand) {
    const std::array<Card, 5> board = board_of("2c", "7d", "Jh", "4s", "9c");
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = 0; b < kDeckSize; ++b) {
            const Card x(a);
            const Card y(b);
            if (a == b || std::find(board.begin(), board.end(), x) != board.end() ||
                std::find(board.begin(), board.end(), y) != board.end()) {
                continue;
            }
            if (HandClass::from_cards(x, y) == hand) return {x, y};
        }
    }
    throw std::logic_error("no cards for hand");
}

TEST(Advisor, TrashBehindLimpersDoesNotRaise) {
    int raises = 0;
    const TrainingAdvisor advisor(24, 1000);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(0, cards_for(hand));
        for (int limp = 0; limp < 3; ++limp) ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
        ASSERT_EQ(engine.state().to_act, 0);
        const HintRecommendation hint = advisor.analyze(engine.player_view(0));
        if (is_aggressive(hint.action.type)) {
            ++raises;
            EXPECT_TRUE(should_open(hand, Position::BTN)) << hand.to_string() << ": " << hint.best_action;
        }
    }
    EXPECT_LT(raises, 100);  // It used to be all 169

    const HintRecommendation trash = advisor.analyze(six_max_limped(hole("7c", "2d")).player_view(0));
    EXPECT_EQ(trash.action.type, ActionType::Fold) << trash.best_action;
    const HintRecommendation aces = advisor.analyze(six_max_limped(hole("Ac", "Ad")).player_view(0));
    EXPECT_TRUE(is_aggressive(aces.action.type)) << aces.best_action;
}

TEST(Advisor, FacingAShoveAgreesWithTheChartEverywhere) {
    const TrainingAdvisor advisor(25, kRollouts);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(2, cards_for(hand));
        while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(2);
        const HintRecommendation hint = advisor.analyze(view);
        const bool chart_folds = advisor_chart_response(view) == PreflopResponse::Fold;
        EXPECT_EQ(hint.action.type, chart_folds ? ActionType::Fold : ActionType::Call)
            << hand.to_string() << ": " << hint.best_action << " " << hint.rationale;
    }
    for (const char* hand : {"Q9o", "K4o", "T8o", "55", "TT"}) {
        EXPECT_EQ(chart_response_for_shove(hand), PreflopResponse::Fold) << hand;
    }
    for (const char* hand : {"AA", "KK", "QQ", "AKo"}) {
        EXPECT_NE(chart_response_for_shove(hand), PreflopResponse::Fold) << hand;
    }
}

// The 169-class tests below take the advisor's own chart as their oracle
// (advisor_chart_response). Facing a single 100 BB raise it answers from its own
// table, advisor_vs_open_ranges, which defends wider than the bots' chart.
TEST(Advisor, AdvisorChartFacingAnOpenUsesItsOwnTable) {
    int differs = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(2, cards_for(hand));
        while (engine.state().to_act != 5) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));
        while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(2);
        const VsRaiseRanges& table = advisor_vs_open_ranges(Position::BB, Position::CO);
        const PreflopResponse expected = table.raise.contains(hand)  ? PreflopResponse::Raise
                                         : table.call.contains(hand) ? PreflopResponse::Call
                                                                     : PreflopResponse::Fold;
        EXPECT_EQ(advisor_chart_response(view), expected) << hand.to_string();
        EXPECT_FALSE(advisor_chart(view).push_fold);
        differs += advisor_chart_response(view) != chart_response(view) ? 1 : 0;
    }
    // The bots' chart is unchanged and narrower: TT, for one, only calls there.
    EXPECT_GT(differs, 20);
}

TEST(Advisor, FacingANormalOpenFollowsTheChartOnWhetherToPlay) {
    const TrainingAdvisor advisor(30, kRollouts);
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(2, cards_for(hand));
        while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
        while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(2);
        const HintRecommendation hint = advisor.analyze(view);
        const bool chart_folds = advisor_chart_response(view) == PreflopResponse::Fold;
        EXPECT_EQ(hint.action.type == ActionType::Fold, chart_folds) << hand.to_string() << ": " << hint.best_action;
    }
}

TEST(Advisor, ACheapCallIsAdvisedWhereTheChartWouldFold) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 2000, 30, 2000, 2000};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[2] = hole("Jc", "6d");
    deal.board = board_of("2c", "7d", "Jh", "4s", "9c");
    deal.board[2] = card("Qh");  // The J is in use
    deal.holes[0] = hole("As", "Ad");
    deal.holes[1] = hole("Ks", "Kd");
    deal.holes[3] = hole("Qs", "Qd");
    deal.holes[4] = hole("Ts", "Td");
    deal.holes[5] = hole("8s", "8d");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(26, kRollouts).analyze(engine.player_view(2));
    EXPECT_EQ(hint.action.type, ActionType::Call) << hint.best_action;
}

TEST(Advisor, AFoldAboveThePriceExplainsItself) {
    // T5o against a minimum raise: the chart folds at 22.2%, under about 40% equity.
    GameEngine engine = six_max(2, hole("Tc", "5d"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 40}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(27, kRollouts).analyze(engine.player_view(2));
    ASSERT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
    ASSERT_GT(hint.equity_pct, hint.pot_odds_pct);
    EXPECT_NE(hint.rationale.find("chart"), std::string::npos) << hint.rationale;
    EXPECT_EQ(hint.rationale.find("highest estimated EV"), std::string::npos) << hint.rationale;
}

TEST(Advisor, AnUnopenedPotMentionsNoMdf) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const HintRecommendation hint = TrainingAdvisor(28, kRollouts).analyze(engine.player_view(3));
    EXPECT_DOUBLE_EQ(hint.mdf_pct, 100.0);
    EXPECT_EQ(hint.rationale.find("MDF"), std::string::npos) << hint.rationale;
}

TEST(Advisor, SizesNeverExceedWhatAnOpponentCanMatch) {
    // Aces with 1600 behind, checked to by someone with 100 behind, in a pot of 800.
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{500, 2000, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Qs", "Qd");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 400}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    const HintRecommendation hint = TrainingAdvisor(29, kRollouts).analyze(engine.player_view(1));
    EXPECT_EQ(hint.action.type, ActionType::Bet) << hint.best_action;
    EXPECT_EQ(hint.action.amount, 100);
    EXPECT_EQ(hint.best_action, "BET 100");
    EXPECT_EQ(hint.raise_25_to, 100);
    EXPECT_FALSE(hint.raise_50_to.has_value());  // The same bet as the 25% one, so listed once
    EXPECT_FALSE(hint.raise_75_to.has_value());
    EXPECT_FALSE(hint.raise_100_to.has_value());
}

TEST(Advisor, AcesRecommendAggressionPreflop) {
    // Not the big blind: folded to, it wins without a decision.
    for (const int seat : {3, 4, 5, 0, 1}) {
        GameEngine engine = six_max(seat, hole("Ac", "Ad"));
        while (engine.state().to_act != seat) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const HintRecommendation hint = TrainingAdvisor(2, kRollouts).analyze(engine.player_view(seat));
        EXPECT_TRUE(is_aggressive(hint.action.type)) << hint.best_action;
    }
}

TEST(Advisor, AcesFacingARaiseStayAggressive) {
    GameEngine engine = six_max(2, hole("Ac", "Ad"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 60}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(3, kRollouts).analyze(engine.player_view(2));
    EXPECT_TRUE(is_aggressive(hint.action.type)) << hint.best_action;
}

TEST(Advisor, TrashFacingABigRaiseFolds) {
    GameEngine engine = six_max(2, hole("7c", "2d"));
    while (engine.state().to_act != 3) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 300}));
    while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const HintRecommendation hint = TrainingAdvisor(4, kRollouts).analyze(engine.player_view(2));
    EXPECT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
    EXPECT_EQ(hint.best_action, "FOLD");
}

TEST(Advisor, TrashFirstInFoldsFromEarlyPosition) {
    GameEngine engine = six_max(3, hole("7c", "2d"));
    const HintRecommendation hint = TrainingAdvisor(5, kRollouts).analyze(engine.player_view(3));
    EXPECT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
}

TEST(Advisor, RiverNutsBetOrRaise) {
    GameEngine engine = heads_up_to_flop(hole("Tc", "Td"), hole("Ah", "Kh"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    const HintRecommendation checked_to = TrainingAdvisor(6, kRollouts).analyze(engine.player_view(1));
    EXPECT_DOUBLE_EQ(checked_to.equity_pct, 100.0);
    EXPECT_TRUE(is_aggressive(checked_to.action.type)) << checked_to.best_action;

    GameEngine facing = heads_up_to_flop(hole("Ah", "Kh"), hole("Tc", "Td"), board_of("Qh", "Jh", "Th", "2c", "3d"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
    }
    ASSERT_TRUE(facing.apply_action({ActionType::Bet, 100}));
    const HintRecommendation raised = TrainingAdvisor(6, kRollouts).analyze(facing.player_view(0));
    EXPECT_TRUE(is_aggressive(raised.action.type)) << raised.best_action;
}

TEST(Advisor, RationaleNamesPositionTierTextureAndNumbers) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 150}));
    const HintRecommendation hint = TrainingAdvisor(7, kRollouts).analyze(engine.player_view(0));
    EXPECT_NE(hint.rationale.find("BTN"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("dry"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("tier"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("pot odds"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("30.0%"), std::string::npos) << hint.rationale;

    GameEngine pre = six_max(3, hole("Ac", "Kd"));
    const std::string text = TrainingAdvisor(7, kRollouts).analyze(pre.player_view(3)).rationale;
    EXPECT_NE(text.find("UTG"), std::string::npos) << text;
    EXPECT_NE(text.find("no board"), std::string::npos) << text;
    EXPECT_NE(text.find("premium"), std::string::npos) << text;
}

TEST(Advisor, IllegalOptionsAreAbsent) {
    // Facing an all-in that covers the stack: only fold or call (the all-in is the call).
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{1000, 200, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Ks", "9h");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    const HintRecommendation hint = TrainingAdvisor(8, kRollouts).analyze(engine.player_view(1));
    EXPECT_FALSE(hint.ev.raise_25.has_value());
    EXPECT_FALSE(hint.ev.raise_50.has_value());
    EXPECT_FALSE(hint.ev.raise_75.has_value());
    EXPECT_FALSE(hint.ev.raise_100.has_value());
    EXPECT_TRUE(hint.ev.check_call.has_value());
    EXPECT_EQ(hint.action.type, ActionType::Call) << hint.best_action;  // AA for a bargain
}

TEST(Advisor, RefusesASeatThatIsNotToAct) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    EXPECT_THROW((void)TrainingAdvisor(9, kRollouts).analyze(engine.player_view(0)), std::invalid_argument);
}

TEST(Advisor, SameViewGivesTheSameHint) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    const TrainingAdvisor advisor(10, kRollouts);
    const PlayerView view = engine.player_view(3);
    const HintRecommendation a = advisor.analyze(view);
    const HintRecommendation b = advisor.analyze(view);
    EXPECT_EQ(a.equity_pct, b.equity_pct);
    EXPECT_EQ(a.best_action, b.best_action);
    EXPECT_EQ(a.rationale, b.rationale);
}

TEST(Advisor, HintsIgnoreOpponentCardsAndUndealtBoard) {
    const auto hint_for = [](const std::array<Card, 2>& opponent, const char* turn, const char* river) {
        GameEngine engine = heads_up_to_flop(opponent, hole("Qh", "Jh"), board_of("Th", "9c", "2h", turn, river));
        return TrainingAdvisor(11, kRollouts).analyze(engine.player_view(1));
    };
    const HintRecommendation a = hint_for(hole("As", "Ad"), "3d", "4d");
    const HintRecommendation b = hint_for(hole("7c", "2s"), "Kd", "Ks");
    EXPECT_EQ(a.equity_pct, b.equity_pct);
    EXPECT_EQ(a.best_action, b.best_action);
    EXPECT_EQ(a.rationale, b.rationale);
    EXPECT_EQ(a.ev.check_call, b.ev.check_call);
    EXPECT_EQ(a.ev.raise_25, b.ev.raise_25);
    EXPECT_EQ(a.ev.raise_50, b.ev.raise_50);
    EXPECT_EQ(a.ev.raise_75, b.ev.raise_75);
    EXPECT_EQ(a.ev.raise_100, b.ev.raise_100);
    EXPECT_EQ(a.ev.all_in, b.ev.all_in);
}

TEST(Advisor, RangeEquityBelowRandomFacingRiverPotBet) {
    // Top pair, weak kicker, in the big blind: it check-calls a bet on the flop and
    // the turn, then faces a pot-sized river bet. The hands that bet three times,
    // the last for the pot, are mostly better.
    GameEngine engine = heads_up_to_flop(hole("As", "Ad"), hole("Kc", "4h"), board_of("Kd", "8s", "3c", "7h", "2d"));
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 70}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 200}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Bet, 740}));
    const HintRecommendation hint = TrainingAdvisor(31, kRollouts).analyze(engine.player_view(1));
    EXPECT_GT(hint.equity_pct, 70.0) << hint.rationale;
    EXPECT_LT(hint.range_equity_pct, hint.equity_pct - 20.0) << hint.rationale;
    EXPECT_NE(hint.rationale.find("against the estimated range"), std::string::npos) << hint.rationale;
    EXPECT_NE(hint.rationale.find("against a random hand"), std::string::npos) << hint.rationale;
}

TEST(Advisor, OutcomesAreConsistent) {
    // Checked to, facing a bet, and facing an all-in, heads-up and multiway.
    GameEngine checked = heads_up_to_flop(hole("Ac", "Ah"), hole("Qs", "Jd"), board_of("Kd", "8s", "3c", "2h", "Jc"));
    const PlayerView first = checked.player_view(1);
    const HintRecommendation unfaced = TrainingAdvisor(32, kRollouts).analyze(first);
    expect_consistent_outcomes(first, unfaced);
    for (std::size_t i = 0; i < kOutcomeCount; ++i) EXPECT_TRUE(unfaced.outcomes[i].has_value()) << i;
    for (std::size_t i = 0; i + 1 < kOutcomeCount; ++i) {
        // Bigger bets fold out more, and leave Hero worse off when called.
        EXPECT_LE(unfaced.outcomes[i]->fold_pct, unfaced.outcomes[i + 1]->fold_pct) << i;
        EXPECT_GE(unfaced.outcomes[i]->equity_called_pct, unfaced.outcomes[i + 1]->equity_called_pct - 1e-9) << i;
    }

    ASSERT_TRUE(checked.apply_action({ActionType::Bet, 100}));
    const PlayerView facing = checked.player_view(0);
    expect_consistent_outcomes(facing, TrainingAdvisor(32, kRollouts).analyze(facing));

    GameEngine preflop = six_max(3, hole("Ac", "Kd"));
    const PlayerView open = preflop.player_view(3);
    const HintRecommendation first_in = TrainingAdvisor(32, kRollouts).analyze(open);
    expect_consistent_outcomes(open, first_in);
    ASSERT_TRUE(first_in.outcomes[kOutcome25].has_value());
    EXPECT_GT(first_in.outcomes[kOutcome25]->fold_pct, 0.0);

    // Facing an all-in that covers the stack: no raise, so no outcomes at all.
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{1000, 200, 0, 0, 0, 0};
    GameEngine shove(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Ks", "9h");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    shove.start_hand(deal);
    ASSERT_TRUE(shove.apply_action({ActionType::AllIn, 0}));
    const HintRecommendation call_only = TrainingAdvisor(32, kRollouts).analyze(shove.player_view(1));
    expect_consistent_outcomes(shove.player_view(1), call_only);
    for (const std::optional<BetOutcome>& o : call_only.outcomes) EXPECT_FALSE(o.has_value());
}

TEST(Advisor, OptionsAreValidated) {
    EXPECT_THROW(TrainingAdvisor(1, AdvisorOptions{0, 500, 0, 0, false}), std::invalid_argument);
    EXPECT_THROW(TrainingAdvisor(1, AdvisorOptions{100, 0, 0, 0, false}), std::invalid_argument);
    EXPECT_THROW(TrainingAdvisor(1, AdvisorOptions{100, 500, -1, 0, false}), std::invalid_argument);
    EXPECT_THROW(TrainingAdvisor(1, AdvisorOptions{100, 500, 0, -1, false}), std::invalid_argument);
    const TrainingAdvisor delegated(1, 5000);
    EXPECT_EQ(delegated.options().rollouts, 5000);
    EXPECT_EQ(delegated.options().range_rollouts, 3000);
    EXPECT_EQ(delegated.options().flop_rivers_per_turn, 0);
    EXPECT_EQ(TrainingAdvisor(1, 100).options().range_rollouts, 100);
}

// Same view, any thread count: the same hint, to the bit.
TEST(Advisor, ThreadCountDoesNotChangeTheHint) {
    GameEngine engine = six_max(3, hole("Ac", "Kd"));
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    while (engine.state().street == Street::Preflop) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    ASSERT_TRUE(engine.state().to_act.has_value());
    const PlayerView view = engine.player_view(*engine.state().to_act);
    const HintRecommendation one = TrainingAdvisor(33, AdvisorOptions{kRollouts, 3000, 0, 1, true}).analyze(view);
    const HintRecommendation three = TrainingAdvisor(33, AdvisorOptions{kRollouts, 3000, 0, 3, true}).analyze(view);
    EXPECT_EQ(one.range_equity_pct, three.range_equity_pct);
    EXPECT_EQ(one.equity_pct, three.equity_pct);
    EXPECT_EQ(one.base_ev.check_call, three.base_ev.check_call);
    EXPECT_EQ(one.base_ev.raise_50, three.base_ev.raise_50);
    EXPECT_EQ(one.best_action, three.best_action);
    // One thread runs the lines in order, more run them side by side.
    for (std::size_t i = 0; i < one.versus.size(); ++i) {
        ASSERT_TRUE(one.versus[i].has_value());
        ASSERT_TRUE(three.versus[i].has_value());
        EXPECT_EQ(one.versus[i]->best_action, three.versus[i]->best_action);
        EXPECT_EQ(one.versus[i]->range_equity_pct, three.versus[i]->range_equity_pct);
        EXPECT_EQ(one.versus[i]->ev.check_call, three.versus[i]->ev.check_call);
        EXPECT_EQ(one.versus[i]->ev.raise_50, three.versus[i]->ev.raise_50);
        EXPECT_EQ(one.versus[i]->why, three.versus[i]->why);
    }
}

// Every field, in every state random play reaches.
TEST(Advisor, FieldsInRangeAndBestActionLegalAcrossRandomStates) {
    std::mt19937_64 rng(4242);
    const TrainingAdvisor advisor(12, kFastOptions);
    int checked = 0;
    for (int game = 0; game < 120; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        std::array<int, kMaxSeats> stacks{};
        for (int& stack : stacks) stack = rng() % 4 == 0 ? 1 + static_cast<int>(rng() % 60) : 100 + static_cast<int>(rng() % 3000);
        config.stacks = stacks;
        GameEngine engine(config);
        for (int hand = 0; hand < 6 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            while (engine.state().to_act) {
                const PlayerView view = engine.player_view(*engine.state().to_act);
                const HintRecommendation hint = advisor.analyze(view);
                ++checked;

                EXPECT_GE(hint.equity_pct, 0.0);
                EXPECT_LE(hint.equity_pct, 100.0);
                EXPECT_GE(hint.pot_odds_pct, 0.0);
                EXPECT_LE(hint.pot_odds_pct, 100.0);
                EXPECT_GE(hint.mdf_pct, 0.0);
                EXPECT_LE(hint.mdf_pct, 100.0);
                EXPECT_GE(hint.spr, 0.0);
                EXPECT_FALSE(hint.best_action.empty());
                EXPECT_FALSE(hint.rationale.empty());

                EXPECT_TRUE(is_legal(view.legal, hint.action)) << hint.best_action;
                if (hint.action.type == ActionType::Bet || hint.action.type == ActionType::Raise) {
                    EXPECT_EQ(hint.action.amount, clamp_raise_to(view, hint.action.amount)) << hint.best_action;
                }
                if (hint.action.type == ActionType::Fold && hint.equity_pct > hint.pot_odds_pct) {
                    EXPECT_NE(hint.rationale.find("overstates"), std::string::npos) << hint.rationale;
                }
                EXPECT_DOUBLE_EQ(ev_of_action(hint), best_listed_ev(hint.ev)) << hint.best_action;
                expect_consistent_outcomes(view, hint);

                if (!view.legal.allows(ActionType::Bet) && !view.legal.allows(ActionType::Raise)) {
                    EXPECT_FALSE(hint.ev.raise_25.has_value());
                    EXPECT_FALSE(hint.ev.raise_50.has_value());
                    EXPECT_FALSE(hint.ev.raise_75.has_value());
                    EXPECT_FALSE(hint.ev.raise_100.has_value());
                }
                if (!view.legal.allows(ActionType::AllIn)) { EXPECT_FALSE(hint.ev.all_in.has_value()); }

                const LegalActions& legal = view.legal;
                std::vector<Action> options;
                if (legal.allows(ActionType::Check)) options.push_back({ActionType::Check, 0});
                if (legal.allows(ActionType::Call)) options.push_back({ActionType::Call, 0});
                if (legal.allows(ActionType::Fold)) options.push_back({ActionType::Fold, 0});
                if (legal.allows(ActionType::AllIn)) options.push_back({ActionType::AllIn, 0});
                if (legal.allows(ActionType::Raise)) options.push_back({ActionType::Raise, legal.min_raise_to});
                if (legal.allows(ActionType::Bet)) options.push_back({ActionType::Bet, legal.min_raise_to});
                ASSERT_TRUE(engine.apply_action(options[static_cast<std::size_t>(rng() % options.size())]));
            }
        }
    }
    EXPECT_GT(checked, 200);
}

// The failure modes a single-street formula drifts into, which a trainer must not teach.
TEST(Advisor, PreflopAdviceAgreesWithTheChartsInClearCases) {
    const auto first_in = [](int seat, const std::array<Card, 2>& h) {
        GameEngine engine = six_max(seat, h);
        while (engine.state().to_act != seat) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        return TrainingAdvisor(14, kRollouts).analyze(engine.player_view(seat)).action.type;
    };
    EXPECT_TRUE(is_aggressive(first_in(3, hole("Ac", "Kd"))));  // Not a fold because five random hands out-draw it
    EXPECT_TRUE(is_aggressive(first_in(3, hole("Tc", "Td"))));
    EXPECT_EQ(first_in(3, hole("Kc", "Td")), ActionType::Fold);
    EXPECT_EQ(first_in(0, hole("7c", "2d")), ActionType::Fold);  // Not even a button steal
}

TEST(Advisor, DoesNotShoveAirOrTopPairDeep) {
    const auto board = board_of("Kd", "8s", "3c", "2h", "Jd");

    // Air may take a small stab against ranges that fold some of the time, but never shoves.
    GameEngine air = heads_up_to_flop(hole("Ac", "Ah"), hole("5c", "4d"), board);
    const HintRecommendation stab = TrainingAdvisor(15, kRollouts).analyze(air.player_view(1));
    EXPECT_NE(stab.action.type, ActionType::AllIn) << stab.best_action;
    if (stab.action.type == ActionType::Bet) { EXPECT_LE(stab.action.amount, 50) << stab.best_action; }

    GameEngine air_facing = heads_up_to_flop(hole("5c", "4d"), hole("Ac", "Ah"), board);
    ASSERT_TRUE(air_facing.apply_action({ActionType::Bet, 150}));
    EXPECT_EQ(TrainingAdvisor(15, kRollouts).analyze(air_facing.player_view(0)).action.type, ActionType::Fold);

    GameEngine top_pair = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ah"), board);
    ASSERT_TRUE(top_pair.apply_action({ActionType::Bet, 150}));
    EXPECT_NE(TrainingAdvisor(15, kRollouts).analyze(top_pair.player_view(0)).action.type, ActionType::AllIn);
}

// --- Bet sizing ----------------------------------------------------------------

// Heads-up, checked to on the flop: pot 200, 1900 behind each.
HintRecommendation checked_to_on_flop(const char* a, const char* b, const char* f1, const char* f2, const char* f3) {
    // The opponent's cards and later streets are invisible to the advisor.
    std::vector<Card> used{card(a), card(b), card(f1), card(f2), card(f3)};
    std::vector<Card> spare;
    for (int id = 0; id < kDeckSize && spare.size() < 4; ++id) {
        if (std::find(used.begin(), used.end(), Card(id)) == used.end()) spare.push_back(Card(id));
    }
    GameEngine engine = heads_up_to_flop({spare[0], spare[1]}, hole(a, b),
                                         {card(f1), card(f2), card(f3), spare[2], spare[3]});
    return TrainingAdvisor(40, 4000).analyze(engine.player_view(1));
}

TEST(AdvisorSizing, EachSizeIsRecommendedInItsSpot) {
    const auto expect_bet = [](const HintRecommendation& hint, int amount, const char* what) {
        EXPECT_EQ(hint.action.type, ActionType::Bet) << what << ": " << hint.best_action;
        EXPECT_EQ(hint.action.amount, amount) << what << ": " << hint.best_action << " | " << hint.rationale;
    };
    expect_bet(checked_to_on_flop("As", "3d", "Kd", "8s", "3c"), 50, "thin value, dry");
    expect_bet(checked_to_on_flop("Ks", "Qh", "Kd", "8s", "3c"), 100, "solid value, dry");
    expect_bet(checked_to_on_flop("Ks", "Jd", "Jh", "7h", "2c"), 150, "solid value, wet");
    expect_bet(checked_to_on_flop("Ah", "Kh", "Jh", "7h", "2c"), 150, "flush draw, wet");
    expect_bet(checked_to_on_flop("Js", "Jc", "Jh", "Th", "9c"), 200, "set, dynamic");
    expect_bet(checked_to_on_flop("Ah", "7h", "9h", "5h", "2h"), 50, "flush, monotone");
}

TEST(AdvisorSizing, VeryStrongRiverHandsBetPot) {
    GameEngine engine = heads_up_to_flop(hole("Ah", "Qd"), hole("8c", "8d"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    const HintRecommendation hint = TrainingAdvisor(41, 4000).analyze(engine.player_view(1));
    EXPECT_EQ(hint.action.type, ActionType::Bet) << hint.best_action;
    EXPECT_EQ(hint.action.amount, 200) << hint.best_action;
    EXPECT_NE(hint.rationale.find("river"), std::string::npos) << hint.rationale;
}

TEST(AdvisorSizing, ShortStacksCanStillShove) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{300, 300, 0, 0, 0, 0};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Qs", "Qd");
    deal.holes[1] = hole("8c", "8d");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::Raise, 100}));
    ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));
    const HintRecommendation hint = TrainingAdvisor(42, 4000).analyze(engine.player_view(1));  // 200 behind, pot 200
    EXPECT_EQ(hint.action.type, ActionType::AllIn) << hint.best_action << " | " << hint.rationale;
}

TEST(AdvisorSizing, DynamicBoardsAreClassified) {
    const auto dynamic = [](std::initializer_list<const char*> cards) {
        std::vector<Card> board;
        for (const char* c : cards) board.push_back(card(c));
        return classify_board(board).dynamic;
    };
    EXPECT_TRUE(dynamic({"Jh", "Th", "9c"}));   // Connected with a flush draw
    EXPECT_TRUE(dynamic({"Jc", "Td", "9s"}));   // Connected alone
    EXPECT_TRUE(dynamic({"Jh", "9h", "7c"}));   // Flush draw and a straight window
    EXPECT_TRUE(dynamic({"Ah", "2c", "3d"}));   // The low ace connects
    EXPECT_FALSE(dynamic({"Kd", "8s", "3c"}));  // Dry
    EXPECT_FALSE(dynamic({"Jh", "7h", "2c"}));  // A flush draw alone
    EXPECT_FALSE(dynamic({"Jc", "9d", "7s"}));  // A straight window alone
    EXPECT_FALSE(dynamic({"Jc", "Td", "9s", "2h", "3d"}));  // No card to come
}

// Random spots, sampling what the rule really does across hands and boards.
TEST(AdvisorSizing, EverySizeIsUsedAndAdviceStaysSane) {
    std::mt19937_64 rng(777);
    const TrainingAdvisor advisor(43, AdvisorOptions{300, 500, 1, 1, false});
    std::array<int, 5> advised{};
    std::array<int, 5> by_base_ev{};
    int spots = 0;
    int bets = 0;

    for (int i = 0; i < 1500; ++i) {
        std::array<Card, kDeckSize> deck;
        for (int id = 0; id < kDeckSize; ++id) deck[static_cast<std::size_t>(id)] = Card(id);
        std::shuffle(deck.begin(), deck.end(), rng);
        const std::array<Card, 2> mine{deck[0], deck[1]};
        const std::array<Card, 5> board{deck[4], deck[5], deck[6], deck[7], deck[8]};
        GameEngine engine = heads_up_to_flop({deck[2], deck[3]}, mine, board);
        const int streets = static_cast<int>(rng() % 3);
        for (int street = 0; street < streets; ++street) {
            ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
            ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
        }
        const PlayerView view = engine.player_view(1);
        const HintRecommendation hint = advisor.analyze(view);
        ++spots;
        EXPECT_DOUBLE_EQ(ev_of_action(hint), best_listed_ev(hint.ev)) << hint.best_action;
        expect_consistent_outcomes(view, hint);

        const std::array<std::optional<int>, 4> sizes{hint.raise_25_to, hint.raise_50_to, hint.raise_75_to, hint.raise_100_to};
        for (std::size_t a = 0; a < sizes.size(); ++a) {
            for (std::size_t b = a + 1; b < sizes.size(); ++b) {
                if (sizes[a] && sizes[b]) { EXPECT_NE(*sizes[a], *sizes[b]); }
            }
        }

        // Fit must not start a bet the model would not make.
        const std::array<std::optional<double>, 4> base{hint.base_ev.raise_25, hint.base_ev.raise_50, hint.base_ev.raise_75, hint.base_ev.raise_100};
        double best_base = -1e18;
        std::size_t best_rung = 0;
        for (std::size_t r = 0; r < base.size(); ++r) {
            if (base[r] && *base[r] > best_base) {
                best_base = *base[r];
                best_rung = r;
            }
        }
        if (hint.action.type == ActionType::Check) {
            EXPECT_GE(*hint.base_ev.check_call, best_base);
            continue;
        }
        ASSERT_EQ(hint.action.type, ActionType::Bet) << hint.best_action;
        EXPECT_GT(best_base, *hint.base_ev.check_call);
        ++bets;
        ++by_base_ev[best_rung];

        const auto rung = std::find(sizes.begin(), sizes.end(), std::optional<int>(hint.action.amount));
        ASSERT_NE(rung, sizes.end());
        ++advised[static_cast<std::size_t>(rung - sizes.begin())];

        const BoardTexture texture = classify_board(view.board);
        const HandRead read = read_hand(mine, view.board);
        if (texture.wet && texture.dynamic && !texture.paired && !texture.monotone && is_nutted(read, view.board) && hint.equity_pct >= 75.0) {
            EXPECT_NE(rung - sizes.begin(), 0) << hint.best_action;
        }
    }

    std::printf("AdvisorSizing over %d spots, %d bets:\n", spots, bets);
    std::printf("  advised     25%%: %d  50%%: %d  75%%: %d  100%%: %d\n", advised[0], advised[1], advised[2], advised[3]);
    std::printf("  model alone 25%%: %d  50%%: %d  75%%: %d  100%%: %d\n", by_base_ev[0], by_base_ev[1], by_base_ev[2], by_base_ev[3]);
    for (int i = 0; i < 4; ++i) {
        EXPECT_GT(advised[static_cast<std::size_t>(i)], 0) << "rung " << i;
        EXPECT_LT(advised[static_cast<std::size_t>(i)], bets * 7 / 10) << "rung " << i;
    }
}

TEST(AdvisorSizing, QuickBetsMatchAdvisorSizes) {
    const auto check = [](const PlayerView& view, const HintRecommendation& hint) {
        const int cap = clamp_raise_to(view, view.legal.max_raise_to);
        const std::array<std::pair<double, std::optional<int>>, 4> rungs{{{0.25, hint.raise_25_to},
                                                                          {0.50, hint.raise_50_to},
                                                                          {0.75, hint.raise_75_to},
                                                                          {1.00, hint.raise_100_to}}};
        for (const auto& [fraction, to] : rungs) {
            const int quick = quick_bet_to(view.legal, view.current_bet, view.total_pot, fraction, cap);
            EXPECT_EQ(quick, clamp_raise_to(view, raise_to_for_pot_fraction(view, fraction)));
            if (to) { EXPECT_EQ(quick, *to); }
        }
    };

    GameEngine normal = heads_up_to_flop(hole("Ac", "Ah"), hole("8c", "8d"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    check(normal.player_view(1), TrainingAdvisor(44, 500).analyze(normal.player_view(1)));

    ASSERT_TRUE(normal.apply_action({ActionType::Bet, 60}));  // Facing a bet
    check(normal.player_view(0), TrainingAdvisor(44, 500).analyze(normal.player_view(0)));

    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{500, 2000, 0, 0, 0, 0};
    GameEngine short_stack(config);
    ScriptedDeal deal;
    deal.holes[0] = hole("Qs", "Qd");
    deal.holes[1] = hole("Ac", "Ad");
    deal.board = board_of("Kd", "8s", "3c", "2h", "Jd");
    short_stack.start_hand(deal);
    ASSERT_TRUE(short_stack.apply_action({ActionType::Raise, 400}));
    ASSERT_TRUE(short_stack.apply_action({ActionType::Call, 0}));
    const PlayerView capped = short_stack.player_view(1);
    check(capped, TrainingAdvisor(44, 500).analyze(capped));
    EXPECT_EQ(quick_bet_to(capped.legal, capped.current_bet, capped.total_pot, 1.0, clamp_raise_to(capped, capped.legal.max_raise_to)), 100);
}

// --- Archetype lines -------------------------------------------------------------

const AdviceLine& versus(const HintRecommendation& hint, Archetype type) {
    for (const std::optional<AdviceLine>& line : hint.versus) {
        if (line && line->versus == type) return *line;
    }
    throw std::logic_error("no line for that type");
}

void expect_same_optional(const std::optional<double>& a, const std::optional<double>& b, const char* what) {
    ASSERT_EQ(a.has_value(), b.has_value()) << what;
    if (a) { EXPECT_EQ(*a, *b) << what; }  // Bit-identical, not merely close
}

void expect_same_baseline(const HintRecommendation& a, const HintRecommendation& b) {
    EXPECT_EQ(a.best_action, b.best_action);
    EXPECT_EQ(a.action.type, b.action.type);
    EXPECT_EQ(a.action.amount, b.action.amount);
    EXPECT_EQ(a.equity_pct, b.equity_pct);
    EXPECT_EQ(a.range_equity_pct, b.range_equity_pct);
    EXPECT_EQ(a.pot_odds_pct, b.pot_odds_pct);
    EXPECT_EQ(a.mdf_pct, b.mdf_pct);
    EXPECT_EQ(a.spr, b.spr);
    for (const auto& [x, y] : {std::pair{&a.ev, &b.ev}, std::pair{&a.base_ev, &b.base_ev}}) {
        expect_same_optional(x->fold, y->fold, "fold");
        expect_same_optional(x->check_call, y->check_call, "check_call");
        expect_same_optional(x->raise_25, y->raise_25, "raise_25");
        expect_same_optional(x->raise_50, y->raise_50, "raise_50");
        expect_same_optional(x->raise_75, y->raise_75, "raise_75");
        expect_same_optional(x->raise_100, y->raise_100, "raise_100");
        expect_same_optional(x->all_in, y->all_in, "all_in");
    }
    EXPECT_EQ(a.raise_25_to, b.raise_25_to);
    EXPECT_EQ(a.raise_50_to, b.raise_50_to);
    EXPECT_EQ(a.raise_75_to, b.raise_75_to);
    EXPECT_EQ(a.raise_100_to, b.raise_100_to);
    for (std::size_t i = 0; i < kOutcomeCount; ++i) {
        ASSERT_EQ(a.outcomes[i].has_value(), b.outcomes[i].has_value()) << i;
        if (!a.outcomes[i]) continue;
        EXPECT_EQ(a.outcomes[i]->fold_pct, b.outcomes[i]->fold_pct) << i;
        EXPECT_EQ(a.outcomes[i]->raise_pct, b.outcomes[i]->raise_pct) << i;
        EXPECT_EQ(a.outcomes[i]->equity_called_pct, b.outcomes[i]->equity_called_pct) << i;
        EXPECT_EQ(a.outcomes[i]->equity_raised_pct, b.outcomes[i]->equity_raised_pct) << i;
    }
    EXPECT_EQ(a.rationale, b.rationale);
}

// Random play with the lines on: every line legal and its own best, the same view
// giving the same lines, and no lines when they are switched off.
TEST(Advisor, ArchetypeLinesAreLegalAndDeterministic) {
    constexpr AdvisorOptions kLinesOn{100, 500, 1, 1, true};
    const TrainingAdvisor advisor(70, kLinesOn);
    const TrainingAdvisor again(70, kLinesOn);
    const TrainingAdvisor off(70, kFastOptions);
    std::mt19937_64 rng(7070);
    int checked = 0;
    int differing = 0;
    for (int game = 0; game < 3; ++game) {
        GameConfig config;
        config.seed = rng();
        config.hero_seat = std::nullopt;
        GameEngine engine(config);
        for (int hand = 0; hand < 3 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
            engine.start_hand();
            while (engine.state().to_act) {
                const PlayerView view = engine.player_view(*engine.state().to_act);
                const HintRecommendation hint = advisor.analyze(view);
                ++checked;
                for (const std::optional<AdviceLine>& line : hint.versus) ASSERT_TRUE(line.has_value());
                expect_consistent_lines(view, hint);
                for (const std::optional<AdviceLine>& line : hint.versus) {
                    if (line->action.type != hint.action.type || line->action.amount != hint.action.amount) ++differing;
                }
                if (checked % 8 == 0) {
                    const HintRecommendation repeat = again.analyze(view);
                    for (std::size_t i = 0; i < hint.versus.size(); ++i) {
                        EXPECT_EQ(repeat.versus[i]->best_action, hint.versus[i]->best_action);
                        EXPECT_EQ(repeat.versus[i]->why, hint.versus[i]->why);
                        EXPECT_EQ(repeat.versus[i]->range_equity_pct, hint.versus[i]->range_equity_pct);
                        expect_same_optional(repeat.versus[i]->ev.check_call, hint.versus[i]->ev.check_call, "check_call");
                        expect_same_optional(repeat.versus[i]->ev.raise_50, hint.versus[i]->ev.raise_50, "raise_50");
                    }
                    for (const std::optional<AdviceLine>& line : off.analyze(view).versus) EXPECT_FALSE(line.has_value());
                }
                const LegalActions& legal = view.legal;
                const unsigned roll = static_cast<unsigned>(rng() % 10);
                Action action{ActionType::Fold, 0};
                if (roll < 5 && legal.allows(ActionType::Check)) action = {ActionType::Check, 0};
                else if (roll < 8 && legal.allows(ActionType::Call)) action = {ActionType::Call, 0};
                else if (legal.allows(ActionType::Bet)) action = {ActionType::Bet, legal.min_raise_to};
                else if (legal.allows(ActionType::Raise) && roll < 9) action = {ActionType::Raise, legal.min_raise_to};
                else if (legal.allows(ActionType::Check)) action = {ActionType::Check, 0};
                ASSERT_TRUE(engine.apply_action(action));
            }
        }
    }
    EXPECT_GT(checked, 60);
    EXPECT_GT(differing, 0);  // The lines are not just copies of Baseline
}

TEST(Advisor, BaselineHintUnchangedByArchetypeLines) {
    std::vector<PlayerView> views;
    GameEngine flop = six_max(3, hole("Ac", "Kd"));
    ASSERT_TRUE(flop.apply_action({ActionType::Raise, 50}));
    ASSERT_TRUE(flop.apply_action({ActionType::Call, 0}));
    ASSERT_TRUE(flop.apply_action({ActionType::Call, 0}));
    views.push_back(flop.player_view(*flop.state().to_act));  // Preflop, facing an open and a call
    while (flop.state().street == Street::Preflop) ASSERT_TRUE(flop.apply_action({ActionType::Call, 0}));
    views.push_back(flop.player_view(*flop.state().to_act));  // A multiway flop
    GameEngine river = heads_up_to_flop(hole("Ac", "Ah"), hole("Qs", "Jd"), board_of("Kd", "8s", "3c", "2h", "Jc"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(river.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(river.apply_action({ActionType::Check, 0}));
    }
    views.push_back(river.player_view(1));  // River, checked to
    ASSERT_TRUE(river.apply_action({ActionType::Bet, 150}));
    views.push_back(river.player_view(0));  // River, facing a bet
    for (const PlayerView& view : views) {
        const HintRecommendation on = TrainingAdvisor(71, AdvisorOptions{kRollouts, 3000, 0, 0, true}).analyze(view);
        const HintRecommendation off = TrainingAdvisor(71, AdvisorOptions{kRollouts, 3000, 0, 0, false}).analyze(view);
        ASSERT_TRUE(on.versus[0].has_value());
        expect_same_baseline(on, off);
    }
}

// Kd8s3c2hJd, heads-up after a raise and a call: Hero is the big blind, first to
// act, after `checked_streets` streets checked through.
GameEngine kd8s3c_checked_to(const std::array<Card, 2>& mine, int checked_streets) {
    GameEngine engine = heads_up_to_flop(hole("Ac", "Ah"), mine, board_of("Kd", "8s", "3c", "2h", "Jd"));
    for (int street = 0; street < checked_streets; ++street) {
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Check, 0}));
    }
    return engine;
}

TEST(Advisor, VsStationBluffsLessAndValueBetsThinner) {
    const TrainingAdvisor advisor(72, kRollouts);

    // Air on the flop: Baseline stabs, but a Station continues too often for that.
    GameEngine air = kd8s3c_checked_to(hole("5c", "4d"), 0);
    const HintRecommendation stab = advisor.analyze(air.player_view(1));
    ASSERT_EQ(stab.action.type, ActionType::Bet) << stab.best_action;
    const AdviceLine& station_air = versus(stab, Archetype::Station);
    EXPECT_EQ(station_air.action.type, ActionType::Check) << station_air.best_action << " | " << station_air.why;
    EXPECT_LT(station_air.outcomes[kOutcome25]->fold_pct, stab.outcomes[kOutcome25]->fold_pct - 10.0);
    EXPECT_NE(station_air.why.find("fold equity"), std::string::npos) << station_air.why;

    // Ace-high on the river (A3 on Kd8s3c2hJd: a pair of threes): thin value. A
    // Station calls with worse, so the bet stays; a Nit only calls with better.
    GameEngine thin = kd8s3c_checked_to(hole("As", "3d"), 2);
    const HintRecommendation river = advisor.analyze(thin.player_view(1));
    const AdviceLine& station = versus(river, Archetype::Station);
    const AdviceLine& nit = versus(river, Archetype::Nit);
    EXPECT_EQ(station.action.type, ActionType::Bet) << station.best_action << " | " << station.why;
    EXPECT_EQ(nit.action.type, ActionType::Check) << nit.best_action << " | " << nit.why;
    EXPECT_GT(station.outcomes[kOutcome25]->equity_called_pct, nit.outcomes[kOutcome25]->equity_called_pct + 20.0);

    // Solid value (top pair, good kicker): a Station's value bet is a rung bigger,
    // a Nit's a rung smaller, than Baseline's.
    GameEngine value = kd8s3c_checked_to(hole("Ks", "Qh"), 0);
    const HintRecommendation top_pair = advisor.analyze(value.player_view(1));
    ASSERT_EQ(top_pair.action.type, ActionType::Bet) << top_pair.best_action;
    ASSERT_EQ(top_pair.action.amount, 100) << top_pair.best_action;
    EXPECT_EQ(versus(top_pair, Archetype::Station).action.amount, 150) << versus(top_pair, Archetype::Station).best_action;
    EXPECT_EQ(versus(top_pair, Archetype::Nit).action.amount, 50) << versus(top_pair, Archetype::Nit).best_action;
    EXPECT_NE(versus(top_pair, Archetype::Station).why.find("bigger"), std::string::npos)
        << versus(top_pair, Archetype::Station).why;
}

TEST(Advisor, VsNitFoldsMoreToBigBets) {
    const TrainingAdvisor advisor(73, kRollouts);

    // Every size folds out more of a Nit's range than of Baseline's.
    GameEngine checked = kd8s3c_checked_to(hole("As", "3d"), 2);
    const HintRecommendation unfaced = advisor.analyze(checked.player_view(1));
    const AdviceLine& nit_unfaced = versus(unfaced, Archetype::Nit);
    for (std::size_t i = 0; i < kOutcomeCount; ++i) {
        ASSERT_TRUE(unfaced.outcomes[i].has_value()) << i;
        EXPECT_GT(nit_unfaced.outcomes[i]->fold_pct, unfaced.outcomes[i]->fold_pct) << i;
    }

    // The same pair of threes facing a pot-sized river bet after two checked
    // streets: a bluff-catcher against Baseline, beaten by a Nit's betting range.
    GameEngine facing = heads_up_to_flop(hole("As", "3d"), hole("Ac", "Ah"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    for (int street = 0; street < 2; ++street) {
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
        ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
    }
    ASSERT_TRUE(facing.apply_action({ActionType::Bet, 200}));
    const HintRecommendation catcher = advisor.analyze(facing.player_view(0));
    EXPECT_EQ(catcher.action.type, ActionType::Call) << catcher.best_action;
    const AdviceLine& nit = versus(catcher, Archetype::Nit);
    EXPECT_EQ(nit.action.type, ActionType::Fold) << nit.best_action << " | " << nit.why;
    EXPECT_LT(nit.range_equity_pct, catcher.pot_odds_pct);
    EXPECT_GT(catcher.range_equity_pct, catcher.pot_odds_pct);
    EXPECT_NE(nit.why.find("pot odds"), std::string::npos) << nit.why;
}

// --- Stage 6: push/fold, positional 3-bets and players still to act ------------

TEST(Advisor, ShortStackFirstInIsPushOrFold) {
    // 10 BB effective, folded to Hero: every class jams or folds, as the table says.
    const TrainingAdvisor advisor(61, kFastOptions);
    for (const int seat : {3, 5, 0, 1}) {
        int jams = 0;
        for (const HandClass hand : hand_classes_by_strength()) {
            GameEngine engine = six_max(seat, cards_for(hand), 5, 200);
            while (engine.state().to_act != seat) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            const PlayerView view = engine.player_view(seat);
            ASSERT_TRUE(advisor_chart(view).push_fold);
            const HintRecommendation hint = advisor.analyze(view);
            const bool table_jams = push_fold_response(hand, *view.position, 10, std::nullopt) == PreflopResponse::Raise;
            EXPECT_EQ(hint.action.type, table_jams ? ActionType::AllIn : ActionType::Fold)
                << seat << " " << hand.to_string() << ": " << hint.best_action;
            jams += hint.action.type == ActionType::AllIn ? hand.combos() : 0;
        }
        EXPECT_NEAR(100.0 * jams / kTotalCombos, push_fold_jam_range(*six_max(seat, hole("Ac", "Ad")).player_view(seat).position, 10)->percent(), 1e-9);
    }
    // At 16 BB the table no longer applies, and a normal raise is advised again.
    GameEngine deeper = six_max(0, hole("Ac", "Kd"), 5, 320);
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(deeper.apply_action({ActionType::Fold, 0}));
    EXPECT_FALSE(advisor_chart(deeper.player_view(0)).push_fold);
}

TEST(Advisor, ShortStackFirstInWithAntesUsesTheAnteTables) {
    // A big-blind ante of one big blind: 1 BB of dead money, and wider jams.
    const TrainingAdvisor advisor(63, kFastOptions);
    for (const int seat : {3, 0}) {
        int jams = 0;
        for (const HandClass hand : hand_classes_by_strength()) {
            GameEngine engine = six_max(seat, cards_for(hand), 5, 200, AnteType::BigBlind, 20);
            while (engine.state().to_act != seat) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            const PlayerView view = engine.player_view(seat);
            ASSERT_EQ(view.dead_money(), 20);
            const HintRecommendation hint = advisor.analyze(view);
            const bool table_jams =
                push_fold_response(hand, *view.position, 10, std::nullopt, kMaxSeats, 1.0) == PreflopResponse::Raise;
            EXPECT_EQ(hint.action.type, table_jams ? ActionType::AllIn : ActionType::Fold)
                << seat << " " << hand.to_string() << ": " << hint.best_action;
            jams += hint.action.type == ActionType::AllIn ? hand.combos() : 0;
        }
        const Position position = *six_max(seat, hole("Ac", "Ad")).player_view(seat).position;
        const double with_antes = push_fold_jam_range(position, 10, kMaxSeats, 1.0)->percent();
        EXPECT_NEAR(100.0 * jams / kTotalCombos, with_antes, 1e-9);
        EXPECT_GT(with_antes, push_fold_jam_range(position, 10)->percent());
    }
}

TEST(Advisor, ShortStackCallsAJamPerTable) {
    // The BB facing a lone 10 BB jam from the SB and from the BTN.
    const TrainingAdvisor advisor(62, kFastOptions);
    for (const int jammer : {1, 0}) {
        for (const HandClass hand : hand_classes_by_strength()) {
            GameEngine engine = six_max(2, cards_for(hand), 5, 200);
            while (engine.state().to_act != jammer) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
            while (engine.state().to_act != 2) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
            const PlayerView view = engine.player_view(2);
            ASSERT_TRUE(advisor_chart(view).push_fold);
            const HintRecommendation hint = advisor.analyze(view);
            const bool table_calls =
                push_fold_response(hand, Position::BB, 10, view.seats[jammer].position) == PreflopResponse::Call;
            EXPECT_EQ(hint.action.type, table_calls ? ActionType::Call : ActionType::Fold)
                << jammer << " " << hand.to_string() << ": " << hint.best_action;
        }
    }
}

// The share of all combos the Baseline advice 3-bets, for `hero` facing an open
// to 50 from `opener` (100 BB), and the classes. No 3-bet is a min-3-bet: each
// is at least twice the open.
double three_bet_share(int hero, int opener, std::string& classes, const char* fold_check = nullptr) {
    AdvisorOptions options;
    options.archetype_lines = false;
    const TrainingAdvisor advisor(63, options);
    int combos = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        GameEngine engine = six_max(hero, cards_for(hand));
        while (engine.state().to_act != opener) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Raise, 50}));
        while (engine.state().to_act != hero) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const HintRecommendation hint = advisor.analyze(engine.player_view(hero));
        if (is_aggressive(hint.action.type)) {
            combos += hand.combos();
            classes += hand.to_string() + " ";
            EXPECT_TRUE(hint.action.type == ActionType::AllIn || hint.action.amount >= 100) << hint.best_action;
        }
        if (fold_check && hand.to_string() == fold_check) {
            EXPECT_EQ(hint.action.type, ActionType::Fold) << hint.best_action;
        }
    }
    return 100.0 * combos / kTotalCombos;
}

TEST(Advisor, BbVsCoOpenThreeBetShareIsPlausible) {
    // Stage 5 found the BB 3-betting almost every hand, 72o included: the CO
    // folded 72% to any 3-bet under the non-positional chart. With the positional
    // table the CO continues about 45%, and the advice follows the advisor's own
    // vs-open table (advisor_vs_open_ranges: 11.5% 3-bets, value plus suited wheel
    // aces and suited connectors), inside the plan's 8-15% band.
    std::string classes;
    const double bb = three_bet_share(2, 5, classes, "72o");
    std::printf("[ Advisor ] BB vs CO 3-bets %.1f%%: %s\n", bb, classes.c_str());
    EXPECT_GE(bb, 8.0);
    EXPECT_LE(bb, 15.0);
    EXPECT_NE(classes.find("AA "), std::string::npos);
    // The BTN against the CO, and the SB against the BTN: plausible too.
    classes.clear();
    const double btn = three_bet_share(0, 5, classes, "72o");
    std::printf("[ Advisor ] BTN vs CO 3-bets %.1f%%: %s\n", btn, classes.c_str());
    EXPECT_GE(btn, 4.0);
    EXPECT_LE(btn, 15.0);
    classes.clear();
    const double sb = three_bet_share(1, 0, classes, "72o");
    std::printf("[ Advisor ] SB vs BTN 3-bets %.1f%%: %s\n", sb, classes.c_str());
    EXPECT_GE(sb, 8.0);
    EXPECT_LE(sb, 20.0);
}

// Hero's range equity against `seats` as infer_ranges(type) reads them, all
// assumed to continue: what the advisor measured before Stage 6 batch C.
double equity_against_all(const PlayerView& view, Archetype type, std::uint8_t seats) {
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges = infer_ranges(view, type);
    RangeEquityOptions options;
    options.seed = 9;
    options.multiway_rollouts = 20000;
    const std::array<LayerRequest, 1> layer{LayerRequest{seats}};
    return 100.0 * equity_vs_ranges(*view.own_hole, view.board, ranges, layer, options).equity[0];
}

TEST(Advisor, PlayersBehindAreNotCertainCallers) {
    const TrainingAdvisor advisor(64, kRollouts);

    // CO AQo facing a 10 BB UTG jam, with the BTN and both blinds still to act: each
    // calls with its push/fold range, so the call is mostly heads-up. It used to be
    // 27.8% "against the range" (four-way) and FOLD.
    {
        GameConfig config;
        config.first_button = 0;
        config.hero_seat = std::nullopt;
        config.stacks = std::array<int, kMaxSeats>{2000, 2000, 2000, 200, 2000, 2000};
        GameEngine engine(config);
        ScriptedDeal deal;
        deal.holes[5] = hole("Ac", "Qd");
        deal.holes[0] = hole("2s", "3s");
        deal.holes[1] = hole("2d", "3d");
        deal.holes[2] = hole("2h", "3h");
        deal.holes[3] = hole("4s", "5s");
        deal.holes[4] = hole("4d", "5d");
        deal.board = board_of("6c", "7c", "8h", "9s", "Td");
        engine.start_hand(deal);
        ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        const PlayerView view = engine.player_view(5);
        const HintRecommendation hint = advisor.analyze(view);
        EXPECT_EQ(hint.action.type, ActionType::Call) << hint.best_action << " " << hint.rationale;
        const double heads_up = equity_against_all(view, Archetype::Baseline, 1u << 3);
        const double everyone = equity_against_all(view, Archetype::Baseline, (1u << 3) | 1u | (1u << 1) | (1u << 2));
        std::printf("[ Advisor ] CO AQo vs a 10 BB UTG jam: %.1f%% (heads-up %.1f%%, all four %.1f%%), %s\n",
                    hint.range_equity_pct, heads_up, everyone, hint.best_action.c_str());
        EXPECT_GT(hint.range_equity_pct, everyone + 10.0);
        EXPECT_LT(hint.range_equity_pct, heads_up + 1.0);
        EXPECT_GT(hint.range_equity_pct, heads_up - 7.0);
    }

    // BTN AJs facing a CO open, blinds still to act: close to the heads-up equity
    // against the CO's range (each blind defends a share of the time), not the
    // three-way equity against every blind range; and the advice plays the hand.
    {
        GameEngine engine = six_max(0, hole("Ac", "Jc"));
        while (engine.state().to_act != 5) ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));
        const PlayerView view = engine.player_view(0);
        const HintRecommendation hint = advisor.analyze(view);
        EXPECT_NE(hint.action.type, ActionType::Fold) << hint.best_action;
        const double heads_up = equity_against_all(view, Archetype::Baseline, 1u << 5);
        const double everyone = equity_against_all(view, Archetype::Baseline, (1u << 5) | (1u << 1) | (1u << 2));
        std::printf("[ Advisor ] BTN AJs vs a CO open: %.1f%% (heads-up %.1f%%, three-way %.1f%%), %s\n",
                    hint.range_equity_pct, heads_up, everyone, hint.best_action.c_str());
        // The blinds defend from the advisor's own tables (the BB 49% of hands against
        // a CO open, the SB 3-bets 10.7% and flats like the BTN), so the field is
        // three-way almost half the time.
        EXPECT_GT(hint.range_equity_pct, heads_up - 9.0);
        EXPECT_LT(hint.range_equity_pct, heads_up);
        EXPECT_GT(hint.range_equity_pct, everyone + 10.0);
        // The archetype lines use the same treatment.
        constexpr std::array<Archetype, 3> kTypes{Archetype::Nit, Archetype::Station, Archetype::Maniac};
        for (std::size_t i = 0; i < kTypes.size(); ++i) {
            ASSERT_TRUE(hint.versus[i]);
            const double line_all = equity_against_all(view, kTypes[i], (1u << 5) | (1u << 1) | (1u << 2));
            EXPECT_GT(hint.versus[i]->range_equity_pct, line_all + 5.0) << to_string(kTypes[i]);
        }
    }

    // Postflop: four-way on the flop, the BB bets and Hero (UTG) calls with the CO
    // and the BTN behind. The range equity counts only the players expected to
    // continue, so it sits above the equity against all three.
    {
        GameEngine engine = six_max(3, hole("Ac", "Qc"));
        ASSERT_TRUE(engine.apply_action({ActionType::Raise, 50}));  // UTG
        ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));     // HJ
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));     // CO
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));     // BTN
        ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));     // SB
        ASSERT_TRUE(engine.apply_action({ActionType::Call, 0}));     // BB
        ASSERT_EQ(engine.state().street, Street::Flop);
        ASSERT_TRUE(engine.apply_action({ActionType::Bet, 100}));    // BB bets half the pot
        const PlayerView view = engine.player_view(3);
        ASSERT_TRUE(view.legal.allows(ActionType::Call));
        const HintRecommendation hint = advisor.analyze(view);
        const double everyone = equity_against_all(view, Archetype::Baseline, (1u << 2) | (1u << 5) | 1u);
        std::printf("[ Advisor ] UTG AQs facing a flop bet with two behind: %.1f%% (all three %.1f%%), %s\n",
                    hint.range_equity_pct, everyone, hint.best_action.c_str());
        EXPECT_GT(hint.range_equity_pct, everyone + 3.0);
        expect_consistent_outcomes(view, hint);
    }
}

// CO AcQd facing a 10 BB UTG jam with the BTN and both blinds still to act: each
// scenario of who calls deals its own hands, the callers from their call ranges
// and the folders from their fold parts (a folder holding call-range cards made
// three-way equity about 2.3 pp too low). The reference rebuilds the call EV from
// the range model's public functions: the nobody-calls scenario at the exact
// per-combo equity, as the advisor scores one opponent, the others by a large
// Monte Carlo.
TEST(Advisor, PlayersBehindWhoFoldHoldTheirFoldParts) {
    GameConfig config;
    config.first_button = 0;
    config.hero_seat = std::nullopt;
    config.stacks = std::array<int, kMaxSeats>{2000, 2000, 2000, 200, 2000, 2000};
    GameEngine engine(config);
    ScriptedDeal deal;
    deal.holes[5] = hole("Ac", "Qd");
    deal.holes[0] = hole("2s", "3s");
    deal.holes[1] = hole("2d", "3d");
    deal.holes[2] = hole("2h", "3h");
    deal.holes[3] = hole("4s", "5s");
    deal.holes[4] = hole("4d", "5d");
    deal.board = board_of("6c", "7c", "8h", "9s", "Td");
    engine.start_hand(deal);
    ASSERT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
    ASSERT_TRUE(engine.apply_action({ActionType::Fold, 0}));
    const PlayerView view = engine.player_view(5);
    ASSERT_EQ(advisor_chart_response(view), PreflopResponse::Call);
    AdvisorOptions options;
    options.archetype_lines = false;
    const HintRecommendation hint = TrainingAdvisor(25, options).analyze(view);
    const double model = *hint.base_ev.check_call - (20.0 + 0.25 * 200.0);  // Less the chart prior

    const ArchetypeParams& params = archetype_params(Archetype::Baseline);
    std::array<std::optional<WeightedRange>, kMaxSeats> ranges = infer_ranges(view, params);
    const CardMask dead = card_mask(std::span<const Card>(*view.own_hole));
    for (std::optional<WeightedRange>& r : ranges) {
        if (r) r->remove_cards(dead);
    }
    const ComboEquities heads_up = hero_vs_each_combo(*view.own_hole, {}, combos_in(*ranges[3]));
    const std::array<int, 3> pending{0, 1, 2};  // BTN, SB, BB
    std::array<double, 3> calls{};
    std::array<WeightedRange, 3> call_ranges;
    std::array<WeightedRange, 3> fold_parts;
    for (std::size_t j = 0; j < pending.size(); ++j) {
        const WeightedRange& range = *ranges[static_cast<std::size_t>(pending[j])];
        PreflopSpot spot;
        spot.villain = *view.seats[static_cast<std::size_t>(pending[j])].position;
        spot.raiser = Position::UTG;
        spot.raises = 1;
        spot.size_ratio = 200.0 / 50.0;
        spot.villain_raised_last = false;
        spot.all_in_bb = 10.0;
        const Response r = predict_preflop_response(range, spot, params);
        ASSERT_EQ(r.raise, 0.0);
        calls[j] = r.call;
        call_ranges[j] = r.call_range;
        for (int combo = 0; combo < kComboCount; ++combo) {
            fold_parts[j].set(combo, std::max(0.0f, range.weight(combo) - r.call_range.weight(combo)));
        }
    }
    const auto reference = [&](bool folders_from_fold_parts) {
        double ev = 0.0;
        for (std::uint32_t mask = 0; mask < 8; ++mask) {
            double chance = 1.0;
            double pot = 430.0;
            std::array<std::optional<WeightedRange>, kMaxSeats> dealt{};
            dealt[3] = ranges[3];
            std::uint8_t field = 1u << 3;
            for (std::size_t j = 0; j < pending.size(); ++j) {
                const auto seat = static_cast<std::size_t>(pending[j]);
                const bool in = ((mask >> j) & 1u) != 0;
                chance *= in ? calls[j] : 1.0 - calls[j];
                if (in) {
                    dealt[seat] = call_ranges[j];
                    field = static_cast<std::uint8_t>(field | (1u << seat));
                    pot += view.seats[seat].stack + view.seats[seat].street_bet > 200 ? 200 - view.seats[seat].street_bet : 0;
                } else {
                    dealt[seat] = folders_from_fold_parts ? fold_parts[j] : call_ranges[j];
                }
            }
            double equity = equity_vs_range(heads_up, *ranges[3]).value();
            if (mask != 0) {
                RangeEquityOptions mc;
                mc.seed = 17 + mask;
                mc.multiway_rollouts = 60000;
                const std::array<LayerRequest, 1> layer{LayerRequest{field}};
                equity = equity_vs_ranges(*view.own_hole, {}, dealt, layer, mc).equity[0];
            }
            ev += chance * (equity * pot - 200.0);
        }
        return ev;
    };
    const double fold_parts_ev = reference(true);
    const double call_ranges_ev = reference(false);
    std::printf("[ Advisor ] CO AQo call vs a 10 BB UTG jam: model %+.1f, reference %+.1f (folders from call "
                "ranges %+.1f)\n",
                model, fold_parts_ev, call_ranges_ev);
    EXPECT_NEAR(model, fold_parts_ev, 2.0);
    EXPECT_GT(fold_parts_ev, call_ranges_ev + 2.0);
}

// A deep jam into a short Hero: the BB has 10 BB, the BTN jams 100 BB with the SB
// (100 BB) still to act behind it. The range model reads that jam at 100 BB (the
// commit range), so the 10 BB push/fold call table, which assumes a 10 BB jam
// and calls 22 and 55, no longer applies, and no other chart does: the range EV
// alone decides. Against JJ+ and AK a 10 BB call needs about 44%.
TEST(Advisor, DeepJamIntoAShortStackHasNoChartPrior) {
    const auto spot = [](const char* a, const char* b, int sb_stack) {
        GameConfig config;
        config.first_button = 0;
        config.hero_seat = std::nullopt;
        config.stacks = std::array<int, kMaxSeats>{2000, sb_stack, 200, 2000, 2000, 2000};
        GameEngine engine(config);
        ScriptedDeal deal;
        deal.holes[2] = hole(a, b);
        deal.board = board_of("2c", "7d", "Jc", "4d", "9c");
        std::array<bool, kDeckSize> used{};
        for (const Card c : deal.board) used[c.id] = true;
        used[card(a).id] = used[card(b).id] = true;
        int next = 0;
        for (int s = 0; s < kMaxSeats; ++s) {
            if (deal.holes[s]) continue;
            std::array<Card, 2> h{};
            for (Card& c : h) {
                while (used[static_cast<std::size_t>(next)]) ++next;
                used[static_cast<std::size_t>(next)] = true;
                c = Card(next);
            }
            deal.holes[s] = h;
        }
        engine.start_hand(deal);
        while (engine.state().to_act != 0) EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::AllIn, 0}));
        EXPECT_TRUE(engine.apply_action({ActionType::Fold, 0}));  // SB
        return engine.player_view(2);
    };
    const TrainingAdvisor advisor(65, kFastOptions);
    for (const auto& [a, b, calls] : std::vector<std::tuple<const char*, const char*, bool>>{
             {"2s", "2h", false}, {"5s", "5h", false}, {"Qs", "Qh", true}, {"Ks", "Kh", true}, {"As", "Ah", true}}) {
        const PlayerView view = spot(a, b, 2000);
        const AdvisorChart chart = advisor_chart(view);
        EXPECT_TRUE(chart.neutral) << a << b;
        EXPECT_FALSE(chart.push_fold) << a << b;
        const HintRecommendation hint = advisor.analyze(view);
        EXPECT_EQ(hint.action.type, calls ? ActionType::Call : ActionType::Fold) << a << b << ": " << hint.best_action;
        // No prior: the call's EV is the range EV alone, equity times what Hero can
        // win less the call.
        const PotMath math = pot_math(view);
        EXPECT_NEAR(*hint.base_ev.check_call,
                    hint.range_equity_pct / 100.0 * (math.contestable + math.to_call) - math.to_call, 1e-6);
    }
    // With the SB short too, the jam is only a 10 BB jam for everyone still in
    // behind it: the push/fold table applies again, and calls 22.
    const PlayerView shallow = spot("2s", "2h", 200);
    EXPECT_TRUE(advisor_chart(shallow).push_fold);
    EXPECT_FALSE(advisor_chart(shallow).neutral);
    EXPECT_EQ(advisor_chart_response(shallow),
              push_fold_response(HandClass::parse("22").value(), Position::BB, 10, Position::BTN));
}

// Run by hand in Release: hint latency by street and opponent count, default
// options with and without the archetype lines, automatic threading and one
// thread. Random play with mostly calls and checks, so many flops are multiway.
TEST(Advisor, DISABLED_LatencyByStreet) {
    for (const bool lines : {false, true})
    for (const int threads : {0, 1}) {
        AdvisorOptions options;
        options.threads = threads;
        options.archetype_lines = lines;
        const TrainingAdvisor advisor(50, options);
        std::array<std::vector<double>, 4> by_street;
        std::array<std::vector<double>, 4> multiway;
        std::mt19937_64 rng(99);
        for (int game = 0; game < 60; ++game) {
            GameConfig config;
            config.seed = rng();
            config.hero_seat = std::nullopt;
            GameEngine engine(config);
            for (int hand = 0; hand < 4 && engine.state().phase == GamePhase::BetweenHands; ++hand) {
                engine.start_hand();
                while (engine.state().to_act) {
                    const PlayerView view = engine.player_view(*engine.state().to_act);
                    const auto start = std::chrono::steady_clock::now();
                    (void)advisor.analyze(view);
                    const double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    by_street[static_cast<std::size_t>(view.street)].push_back(ms);
                    if (view.opponents_in_hand() >= 2) multiway[static_cast<std::size_t>(view.street)].push_back(ms);
                    const LegalActions& legal = view.legal;
                    const unsigned roll = static_cast<unsigned>(rng() % 10);
                    Action action{ActionType::Fold, 0};
                    if (roll < 6 && legal.allows(ActionType::Check)) action = {ActionType::Check, 0};
                    else if (roll < 8 && legal.allows(ActionType::Call)) action = {ActionType::Call, 0};
                    else if (roll < 9 && legal.allows(ActionType::Bet)) action = {ActionType::Bet, legal.min_raise_to};
                    else if (roll < 9 && legal.allows(ActionType::Raise)) action = {ActionType::Raise, legal.min_raise_to};
                    else if (legal.allows(ActionType::Check)) action = {ActionType::Check, 0};
                    ASSERT_TRUE(engine.apply_action(action));
                }
            }
        }
        const auto report = [](const char* name, std::vector<double> v) {
            if (v.empty()) return;
            std::sort(v.begin(), v.end());
            const auto q = [&v](double f) { return v[std::min(v.size() - 1, static_cast<std::size_t>(f * static_cast<double>(v.size())))]; };
            std::printf("  %-14s n=%4zu  p50 %6.2f  p95 %6.2f  max %6.2f ms\n", name, v.size(), q(0.5), q(0.95), v.back());
        };
        // Heads-up after a raise and a call, checked to on each street.
        std::array<std::vector<double>, 4> heads_up;
        for (int i = 0; i < 300; ++i) {
            std::array<Card, kDeckSize> deck;
            for (int id = 0; id < kDeckSize; ++id) deck[static_cast<std::size_t>(id)] = Card(id);
            std::shuffle(deck.begin(), deck.end(), rng);
            GameEngine engine = heads_up_to_flop({deck[2], deck[3]}, {deck[0], deck[1]},
                                                 {deck[4], deck[5], deck[6], deck[7], deck[8]});
            for (int street = 1; street <= 3; ++street) {
                const PlayerView view = engine.player_view(1);
                const auto start = std::chrono::steady_clock::now();
                (void)advisor.analyze(view);
                heads_up[static_cast<std::size_t>(street)].push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                ASSERT_TRUE(engine.apply_action({ActionType::Check, 0}));
                if (street < 3) { ASSERT_TRUE(engine.apply_action({ActionType::Check, 0})); }
            }
        }
        std::printf("archetype_lines=%d threads=%d\n", lines ? 1 : 0, threads);
        report("flop HU", heads_up[1]);
        report("turn HU", heads_up[2]);
        report("river HU", heads_up[3]);
        report("preflop", by_street[0]);
        report("preflop 3+ way", multiway[0]);
        report("flop", by_street[1]);
        report("flop 3+ way", multiway[1]);
        report("turn", by_street[2]);
        report("turn 3+ way", multiway[2]);
        report("river", by_street[3]);
        report("river 3+ way", multiway[3]);
    }
}

// Run by hand: reads the advice for a spread of spots.
TEST(Advisor, DISABLED_PrintSampleHints) {
    const auto show = [](const char* what, const PlayerView& view) {
        const HintRecommendation h = TrainingAdvisor(1).analyze(view);
        std::printf("%s\n  -> %s\n  %s\n  EV fold %.0f call %.0f r25 %.0f r50 %.0f r75 %.0f r100 %.0f shove %.0f\n", what,
                    h.best_action.c_str(), h.rationale.c_str(), h.ev.fold.value_or(-1), h.ev.check_call.value_or(-1),
                    h.ev.raise_25.value_or(-1), h.ev.raise_50.value_or(-1), h.ev.raise_75.value_or(-1),
                    h.ev.raise_100.value_or(-1), h.ev.all_in.value_or(-1));
    };
    for (const auto& h : {hole("Ac", "Ad"), hole("Ac", "Kd"), hole("Tc", "Td"), hole("Kc", "Td"), hole("8h", "7h"), hole("9h", "4d")}) {
        GameEngine utg = six_max(3, h);
        show("UTG first in", utg.player_view(3));
        GameEngine bb = six_max(2, h);
        while (bb.state().to_act != 3) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(bb.apply_action({ActionType::Raise, 60}));
        while (bb.state().to_act != 2) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        show("BB vs 3x UTG", bb.player_view(2));
    }
    const auto b = board_of("Kd", "8s", "3c", "2h", "Jd");
    for (const auto& h : {hole("Ks", "9h"), hole("8c", "8d"), hole("5c", "4d"), hole("Tc", "9c")}) {
        GameEngine checked = heads_up_to_flop(hole("Ac", "Ah"), h, b);
        show("flop, checked to", checked.player_view(1));
        GameEngine facing = heads_up_to_flop(h, hole("Ac", "Ah"), b);
        ASSERT_TRUE(facing.apply_action({ActionType::Bet, 150}));
        show("flop, facing 75%", facing.player_view(0));
    }
}

// Run by hand: Baseline and the three archetype lines for a spread of spots.
void print_lines(const char* what, const HintRecommendation& h) {
    const auto row = [](const char* name, const std::string& best, const EvEstimates& ev, double range_eq,
                        const std::array<std::optional<BetOutcome>, kOutcomeCount>& outcomes) {
        std::printf("  %-8s %-24s eq %5.1f  EV f %6.1f c %6.1f r25 %6.1f r50 %6.1f r75 %6.1f r100 %6.1f ai %6.1f | fold",
                    name, best.c_str(), range_eq, ev.fold.value_or(-1), ev.check_call.value_or(-1),
                    ev.raise_25.value_or(-1), ev.raise_50.value_or(-1), ev.raise_75.value_or(-1),
                    ev.raise_100.value_or(-1), ev.all_in.value_or(-1));
        for (const auto& o : outcomes) std::printf(" %3.0f", o ? o->fold_pct : -1.0);
        std::printf("\n");
    };
    std::printf("%s\n", what);
    row("Baseline", h.best_action, h.ev, h.range_equity_pct, h.outcomes);
    for (const std::optional<AdviceLine>& line : h.versus) {
        if (!line) continue;
        row(std::string(to_string(line->versus)).c_str(), line->best_action, line->ev, line->range_equity_pct,
            line->outcomes);
        std::printf("           why: %s (%zu chars)\n", line->why.c_str(), line->why.size());
    }
}

TEST(Advisor, DISABLED_PrintArchetypeLines) {
    const TrainingAdvisor advisor(60);
    const auto b = board_of("Kd", "8s", "3c", "2h", "Jd");
    for (const auto& h : {hole("5c", "4d"), hole("As", "3d"), hole("Qh", "Ts"), hole("9c", "9h"), hole("Ks", "Qh"),
                          hole("8c", "8d"), hole("As", "Qd")}) {
        GameEngine checked = heads_up_to_flop(hole("Ac", "Ah"), h, b);
        print_lines(("flop checked to " + h[0].to_string() + h[1].to_string()).c_str(),
                    advisor.analyze(checked.player_view(1)));
        for (int street = 0; street < 2; ++street) {
            ASSERT_TRUE(checked.apply_action({ActionType::Check, 0}));
            ASSERT_TRUE(checked.apply_action({ActionType::Check, 0}));
        }
        print_lines("  river checked to", advisor.analyze(checked.player_view(1)));
        GameEngine facing = heads_up_to_flop(h, hole("Ac", "Ah"), b);
        for (int street = 0; street < 2; ++street) {
            ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
            ASSERT_TRUE(facing.apply_action({ActionType::Check, 0}));
        }
        ASSERT_TRUE(facing.apply_action({ActionType::Bet, 200}));
        print_lines("  river facing pot", advisor.analyze(facing.player_view(0)));
    }
    // Preflop: the button first in, and the big blind facing a cutoff open.
    for (const char* text : {"K4o", "Q7o", "J6s", "96s", "T7o", "A2o", "K8o", "Q9o", "65o", "J8o"}) {
        const HandClass hand = HandClass::parse(text).value();
        GameEngine btn = six_max(0, cards_for(hand));
        while (btn.state().to_act != 0) ASSERT_TRUE(btn.apply_action({ActionType::Fold, 0}));
        print_lines((std::string("BTN first in ") + text).c_str(), advisor.analyze(btn.player_view(0)));
    }
    for (const char* text : {"A8o", "KTo", "Q9s", "55", "A5s", "K9o", "J9o", "T8s", "A3o", "Q8o"}) {
        const HandClass hand = HandClass::parse(text).value();
        GameEngine bb = six_max(2, cards_for(hand));
        while (bb.state().to_act != 5) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        ASSERT_TRUE(bb.apply_action({ActionType::Raise, 50}));
        while (bb.state().to_act != 2) ASSERT_TRUE(bb.apply_action({ActionType::Fold, 0}));
        print_lines((std::string("BB vs CO open ") + text).c_str(), advisor.analyze(bb.player_view(2)));
    }
}

TEST(Advisor, DefaultRolloutsPath) {
    GameEngine engine = heads_up_to_flop(hole("Ks", "9h"), hole("Ac", "Ad"), board_of("Kd", "8s", "3c", "2h", "Jd"));
    const HintRecommendation hint = TrainingAdvisor(13).analyze(engine.player_view(1));
    EXPECT_GT(hint.equity_pct, 0.0);
}

}  // namespace
