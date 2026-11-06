#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "core/hand_evaluator.h"
#include "engine/game_engine.h"
#include "range/combos.h"
#include "range/hand_vs_range.h"
#include "range/range_model.h"
#include "range/weighted_range.h"
#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

// Six-handed with the button on seat 0.
constexpr int kBtn = 0;
constexpr int kSb = 1;
constexpr int kBb = 2;
constexpr int kUtg = 3;
constexpr int kHj = 4;
constexpr int kCo = 5;

constexpr std::array<Archetype, kArchetypeCount> kArchetypes{Archetype::Baseline, Archetype::Nit, Archetype::Station,
                                                              Archetype::Maniac};

Card card(const char* text) { return Card::parse(text).value(); }
std::array<Card, 2> hole(const char* a, const char* b) { return {card(a), card(b)}; }
HandClass hand(const char* text) { return HandClass::parse(text).value(); }

std::array<Card, 5> board_of(const char* a, const char* b, const char* c, const char* d, const char* e) {
    return {card(a), card(b), card(c), card(d), card(e)};
}

using Holes = std::array<std::optional<std::array<Card, 2>>, kMaxSeats>;

// Every dealt player without given cards gets spare ones, avoiding the given cards.
GameEngine table(Holes holes, const std::array<Card, 5>& board,
                 std::array<int, kMaxSeats> stacks = {2000, 2000, 2000, 2000, 2000, 2000}) {
    GameConfig config;
    config.first_button = kBtn;
    config.hero_seat = std::nullopt;
    config.stacks = stacks;
    GameEngine engine(config);

    std::array<bool, kDeckSize> used{};
    for (const Card c : board) used[c.id] = true;
    for (const auto& h : holes) {
        if (h) used[(*h)[0].id] = used[(*h)[1].id] = true;
    }
    int next = 0;
    const auto spare = [&] {
        while (used[static_cast<std::size_t>(next)]) ++next;
        used[static_cast<std::size_t>(next)] = true;
        return Card(next);
    };
    ScriptedDeal deal;
    deal.board = board;
    for (int i = 0; i < kMaxSeats; ++i) {
        if (stacks[static_cast<std::size_t>(i)] == 0) continue;
        const auto k = static_cast<std::size_t>(i);
        deal.holes[k] = holes[k] ? *holes[k] : std::array<Card, 2>{spare(), spare()};
    }
    engine.start_hand(deal);
    return engine;
}

constexpr Action F{ActionType::Fold, 0};
constexpr Action X{ActionType::Check, 0};
constexpr Action C{ActionType::Call, 0};
constexpr Action R(int to) { return {ActionType::Raise, to}; }
constexpr Action B(int to) { return {ActionType::Bet, to}; }

void play(GameEngine& engine, std::initializer_list<Action> actions) {
    for (const Action& a : actions) ASSERT_TRUE(engine.apply_action(a)) << to_string(a.type) << " " << a.amount;
}

// BTN opens to 2.5 BB and the BB calls: 110 in the pot on the flop, BB first.
void btn_open_bb_call(GameEngine& engine) { play(engine, {F, F, F, R(50), F, C}); }

const std::array<Card, 5> kDryBoard = board_of("Kh", "7d", "2c", "5s", "Jh");

WeightedRange chart_weights(const HandRange& range, CardMask dead) {
    WeightedRange out = WeightedRange::from_hand_range(range);
    out.remove_cards(dead);
    return out;
}

CardMask dead_for(const PlayerView& view) {
    CardMask dead = card_mask(std::span<const Card>(view.board));
    if (view.own_hole) dead |= card_mask(std::span<const Card>(*view.own_hole));
    return dead;
}

void expect_same(const WeightedRange& a, const WeightedRange& b) {
    for (int combo = 0; combo < kComboCount; ++combo) {
        ASSERT_EQ(a.weight(combo), b.weight(combo)) << "combo " << combo;
    }
}

double class_weight(const WeightedRange& range, const char* text) {
    const HandClass h = hand(text);
    const double total = range.class_mass(h);
    return total / h.combos();
}

// The posterior mass share of combos in the top `share` of `prior`.
double top_mass_share(const WeightedRange& posterior, const WeightedRange& prior, const StreetStrength& s,
                      double share) {
    const auto t = top_share(prior, s);
    double top = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (t[static_cast<std::size_t>(combo)] <= share) top += posterior.weight(combo);
    }
    return top / posterior.total();
}

// --- resize_range ---------------------------------------------------------------

TEST(ResizeRange, KeepsShapeAndMonotone) {
    std::vector<HandRange> charts;
    for (const Position pos : {Position::UTG, Position::HJ, Position::CO, Position::BTN, Position::SB}) {
        charts.push_back(rfi_range(pos));
    }
    charts.push_back(vs_open_ranges(Position::BB, Position::BTN).call);
    charts.push_back(vs_open_ranges(Position::BTN, Position::CO).raise);

    for (const HandRange& chart : charts) {
        const HandRange same = resize_range(chart, 1.0, 100.0);
        EXPECT_TRUE(same.contains_all(chart) && chart.contains_all(same));

        HandRange previous;
        for (int step = 0; step <= 40; ++step) {
            const double mult = 0.075 * step;
            const HandRange r = resize_range(chart, mult, 100.0);
            EXPECT_TRUE(r.contains_all(previous)) << "nesting at " << mult;
            if (mult <= 1.0) {
                EXPECT_TRUE(chart.contains_all(r)) << "shrink leaves the chart at " << mult;
            }
            if (mult >= 1.0) {
                EXPECT_TRUE(r.contains_all(chart)) << "growth drops the chart at " << mult;
            }
            // Nearest class boundary: within half the largest class (6 combos).
            const double target = std::min(chart.combos() * mult, static_cast<double>(kTotalCombos));
            EXPECT_LE(std::abs(r.combos() - target), 6.0 + 1e-9) << "target " << target;
            previous = r;
        }
    }

    // The cap is hard and keeps the shape: the BTN chart cut to 15% is its strongest part.
    const HandRange btn = rfi_range(Position::BTN);
    const HandRange capped = resize_range(btn, 1.0, 15.0);
    EXPECT_LE(capped.percent(), 15.0);
    EXPECT_GT(capped.percent(), 14.0);
    EXPECT_TRUE(btn.contains_all(capped));
    EXPECT_TRUE(resize_range(btn, 3.0, 15.0).contains_all(capped));
    EXPECT_TRUE(capped.contains_all(resize_range(btn, 3.0, 15.0)));

    EXPECT_TRUE(resize_range(btn, 0.0).empty());
    EXPECT_TRUE(resize_range(btn, -1.0).empty());
    EXPECT_TRUE(resize_range(HandRange{}, 2.0).empty());
    EXPECT_EQ(resize_range(btn, 10.0).combos(), kTotalCombos);
    // Shrinking drops the weakest: AA survives a tiny width, and 72o is never added early.
    EXPECT_TRUE(resize_range(btn, 0.02).contains(hand("AA")));
    EXPECT_FALSE(resize_range(rfi_range(Position::UTG), 1.5).contains(hand("72o")));
}

// --- Board strength -------------------------------------------------------------

TEST(StreetStrength, RanksLiveCombosAndReadsDraws) {
    const std::array<Card, 5> river = board_of("Kh", "7d", "2c", "5s", "Jh");
    const StreetStrength s = street_strength(river);
    EXPECT_EQ(s.live_count, 47 * 46 / 2);
    EXPECT_EQ(s.board_size, 5);
    for (int k = 1; k < s.live_count; ++k) {
        ASSERT_GE(s.hs[s.order[static_cast<std::size_t>(k - 1)]], s.hs[s.order[static_cast<std::size_t>(k)]]);
    }
    // The nuts here is a set of kings; the weakest a hand that loses to everything.
    const int kings = combo_index(card("Kc"), card("Kd"));
    EXPECT_GT(s.hs[static_cast<std::size_t>(kings)], 0.99f);
    EXPECT_FALSE(s.live.test(static_cast<std::size_t>(combo_index(card("Kh"), card("Kd")))));
    EXPECT_EQ(s.hs[static_cast<std::size_t>(combo_index(card("Kh"), card("Kd")))], 0.0f);
    for (int combo = 0; combo < kComboCount; ++combo) ASSERT_EQ(s.draw[static_cast<std::size_t>(combo)], kNoDraw);

    const std::array<Card, 3> flop{card("Kh"), card("7h"), card("5c")};
    const StreetStrength f = street_strength(flop);
    EXPECT_EQ(f.draw[static_cast<std::size_t>(combo_index(card("Ah"), card("2h")))], kStrongDraw);
    EXPECT_EQ(f.draw[static_cast<std::size_t>(combo_index(card("8d"), card("6s")))], kStrongDraw);
    EXPECT_EQ(f.draw[static_cast<std::size_t>(combo_index(card("9d"), card("6s")))], kGutshotOnly);
    EXPECT_TRUE(f.holds_pair.test(static_cast<std::size_t>(combo_index(card("5d"), card("2s")))));

    // Equal hands tie exactly, and top_share puts a tie group at its midpoint.
    EXPECT_EQ(s.hs[static_cast<std::size_t>(combo_index(card("Ac"), card("Ad")))],
              s.hs[static_cast<std::size_t>(combo_index(card("Ah"), card("As")))]);
    const auto t = top_share(WeightedRange::uniform(), s);
    EXPECT_LT(t[static_cast<std::size_t>(kings)], 0.01f);
    EXPECT_EQ(t[static_cast<std::size_t>(combo_index(card("Kh"), card("Kd")))], 1.0f);  // Dead
    double mean = 0.0;
    for (const std::uint16_t combo : s.live_order()) mean += t[combo];
    EXPECT_NEAR(mean / s.live_count, 0.5, 1e-6);  // Midpoints of a uniform range average a half

    EXPECT_THROW(street_strength(std::array<Card, 2>{card("Ah"), card("Kd")}), std::invalid_argument);
    EXPECT_THROW(street_strength(std::array<Card, 3>{card("Ah"), card("Ah"), card("Kd")}), std::invalid_argument);
}

// --- Preflop inference ----------------------------------------------------------

TEST(RangeModel, BtnOpenIsTheChart) {
    Holes holes{};
    holes[kSb] = hole("Qs", "Jd");
    GameEngine engine = table(holes, kDryBoard);
    play(engine, {F, F, F, R(50)});
    const PlayerView view = engine.player_view(kSb);

    const WeightedRange expected = chart_weights(rfi_range(Position::BTN), dead_for(view));
    expect_same(infer_range_for(view, kBtn, Archetype::Baseline, true), expected);

    const auto ranges = infer_ranges(view, Archetype::Baseline);
    ASSERT_TRUE(ranges[kBtn].has_value());
    expect_same(*ranges[kBtn], expected);
    EXPECT_FALSE(ranges[kSb].has_value());   // The viewer
    EXPECT_FALSE(ranges[kUtg].has_value());  // Folded
    ASSERT_TRUE(ranges[kBb].has_value());    // Not acted yet: every combo the viewer does not block
    EXPECT_NEAR(ranges[kBb]->total(), 50 * 49 / 2, 1e-9);
}

TEST(RangeModel, BbDefendVsBtnIsChartCallPlusTraps) {
    Holes holes{};
    holes[kBtn] = hole("8c", "8d");
    GameEngine engine = table(holes, board_of("Ah", "9s", "4c", "2d", "Kh"));
    btn_open_bb_call(engine);
    const PlayerView view = engine.player_view(kBtn);
    ASSERT_EQ(view.street, Street::Flop);

    const VsRaiseRanges& chart = vs_open_ranges(Position::BB, Position::BTN);
    const float trap = static_cast<float>(archetype_params(Archetype::Baseline).slowplay);
    WeightedRange expected = WeightedRange::from_hand_range(chart.call);
    for (const HandClass h : hand_classes_by_strength()) {
        if (chart.raise.contains(h)) expected.set_class(h, trap);
    }
    expected.remove_cards(dead_for(view));
    expect_same(*infer_ranges(view, Archetype::Baseline)[kBb], expected);
}

TEST(RangeModel, NitNarrowerStationWiderThanBaseline) {
    const auto masses = [](Archetype a) {
        GameEngine open = table({}, kDryBoard);
        play(open, {R(50)});  // UTG opens
        const double utg_open = infer_range_for(open.player_view(kHj), kUtg, a, false).total();

        GameEngine flat = table({}, kDryBoard);
        btn_open_bb_call(flat);
        const double bb_flat = infer_range_for(flat.player_view(kBtn), kBb, a, false).total();
        return std::array<double, 2>{utg_open, bb_flat};
    };
    const auto base = masses(Archetype::Baseline);
    const auto nit = masses(Archetype::Nit);
    const auto station = masses(Archetype::Station);
    const auto maniac = masses(Archetype::Maniac);
    for (std::size_t k = 0; k < 2; ++k) {
        EXPECT_LT(nit[k], base[k]) << k;
        EXPECT_LT(base[k], station[k]) << k;
    }
    EXPECT_GT(maniac[0], base[0]);
    // Widths match the parameters: UTG opens 16% x 0.6 for the Nit.
    EXPECT_NEAR(nit[0] / base[0], 0.6, 0.05);
    EXPECT_NEAR(station[0] / base[0], 1.4, 0.05);
}

TEST(RangeModel, LimpIsAWindowOutsideTheOpenPlusTraps) {
    GameEngine engine = table({}, kDryBoard);
    play(engine, {C});  // UTG limps
    const WeightedRange r = infer_range_for(engine.player_view(kHj), kUtg, Archetype::Baseline, false);
    const HandRange& open = rfi_range(Position::UTG);
    EXPECT_NEAR(r.total(), 0.02 * kComboCount + 0.3 * HandRange::top_percent(3.5).combos(), 6.0);
    for (const HandClass h : hand_classes_by_strength()) {
        const double w = r.class_mass(h) / h.combos();
        if (preflop_tier(h) == PreflopTier::Premium) {
            EXPECT_NEAR(w, 0.3, 1e-6) << h.to_string();
        } else if (open.contains(h)) {
            EXPECT_EQ(w, 0.0) << h.to_string();
        }
    }
    // The window is right below the open range, not trash.
    EXPECT_EQ(class_weight(r, "72o"), 0.0);
    EXPECT_GT(r.total() - 0.3 * HandRange::top_percent(3.5).combos(), 20.0);
}

TEST(RangeModel, ThreeBetAndFourBetUseTheCharts) {
    GameEngine engine = table({}, kDryBoard);
    play(engine, {F, F, R(50), R(150), F, F, R(330)});  // CO opens, BTN 3-bets, CO 4-bets
    const PlayerView view = engine.player_view(kBtn);
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);

    // The BB is the viewer here only for card removal's sake: use no removal.
    const WeightedRange co = infer_range_for(view, kCo, Archetype::Baseline, false);
    const WeightedRange btn = infer_range_for(engine.player_view(kSb), kBtn, Archetype::Baseline, false);
    const VsRaiseRanges& vs_co = vs_open_ranges(Position::BTN, Position::CO);
    for (const HandClass h : hand_classes_by_strength()) {
        const double b = btn.class_mass(h) / h.combos();
        EXPECT_NEAR(b, vs_co.raise.contains(h) ? 1.0 - p.slowplay : 0.0, 1e-6) << h.to_string();
        const double c = co.class_mass(h) / h.combos();
        const bool four_bets = vs_three_bet_response(h) == PreflopResponse::Raise && rfi_range(Position::CO).contains(h);
        EXPECT_NEAR(c, four_bets ? 1.0 - p.slowplay : 0.0, 1e-6) << h.to_string();
    }

    // Calling the 4-bet: the vs-4-bet call range plus slowplayed 5-bet hands.
    play(engine, {C});
    ASSERT_EQ(engine.state().street, Street::Flop);
    const WeightedRange caller = infer_range_for(engine.player_view(kCo), kBtn, Archetype::Baseline, false);
    EXPECT_NEAR(class_weight(caller, "QQ"), 1.0 - p.slowplay, 1e-6);  // 3-bet at 0.8, then flats a 4-bet at 1
    EXPECT_NEAR(class_weight(caller, "AA"), (1.0 - p.slowplay) * p.slowplay, 1e-6);
    EXPECT_EQ(class_weight(caller, "TT"), 0.0);
}

TEST(RangeModel, ShortAllInWidensAsStacksShrink) {
    const auto shove_mass = [](int stack) {
        GameEngine engine = table({}, kDryBoard, {stack, 2000, 2000, 2000, 2000, 2000});
        play(engine, {F, F, F, R(stack)});  // BTN shoves
        return infer_range_for(engine.player_view(kSb), kBtn, Archetype::Baseline, false).total();
    };
    const double at15 = shove_mass(300);
    const double at10 = shove_mass(200);
    const double at5 = shove_mass(100);
    EXPECT_LT(at15, at10);
    EXPECT_LT(at10, at5);
    EXPECT_NEAR(at10 / kComboCount, 0.40, 0.02);  // Temporary formula: 400 / bb percent

    // A deep shove is the commit range, QQ+ and AK.
    GameEngine deep = table({}, kDryBoard);
    play(deep, {F, F, F, R(2000)});
    const WeightedRange commit = infer_range_for(deep.player_view(kSb), kBtn, Archetype::Baseline, false);
    EXPECT_NEAR(commit.total(), 6 * 4 + 16, 1e-6);  // JJ+ and AK, preflop
    EXPECT_EQ(class_weight(commit, "AKo"), 1.0);
    EXPECT_EQ(class_weight(commit, "TT"), 0.0);

    // Calling a deep shove commits too; calling a short one is wider than the deep commit.
    play(deep, {F, C});
    const WeightedRange caller = infer_range_for(deep.player_view(kBtn), kBb, Archetype::Baseline, false);
    const CardMask board = card_mask(std::span<const Card>(kDryBoard));  // Run out by now
    EXPECT_NEAR(caller.total(), chart_weights(HandRange::parse("JJ+, AKs, AKo"), board).total(), 1e-6);
    GameEngine shallow = table({}, kDryBoard, {200, 2000, 2000, 2000, 2000, 2000});
    play(shallow, {F, F, F, R(200), F, C});
    const WeightedRange short_call = infer_range_for(shallow.player_view(kBtn), kBb, Archetype::Baseline, false);
    // Temporary formula: the top 250 / bb percent, here 25%.
    EXPECT_EQ(short_call.weights(), chart_weights(HandRange::top_percent(25.0), board).weights());
    EXPECT_GT(short_call.total(), caller.total());
}

TEST(RangeModel, BbCheckKeepsEverythingButItsRaises) {
    GameEngine engine = table({}, kDryBoard);
    play(engine, {F, F, F, F, C, X});  // SB completes, BB checks
    const PlayerView view = engine.player_view(kSb);
    ASSERT_EQ(view.street, Street::Flop);
    const WeightedRange bb = infer_range_for(view, kBb, Archetype::Baseline, false);
    EXPECT_NEAR(class_weight(bb, "83o"), 1.0, 1e-6);
    EXPECT_NEAR(class_weight(bb, "AA"), archetype_params(Archetype::Baseline).slowplay, 1e-6);
    // The SB's completion is a limp: a window outside its open range plus traps.
    const WeightedRange sb = infer_range_for(engine.player_view(kBb), kSb, Archetype::Baseline, false);
    EXPECT_EQ(class_weight(sb, "AQs"), 0.0);  // In its open range
    EXPECT_NEAR(class_weight(sb, "QQ"), archetype_params(Archetype::Baseline).limp_trap, 1e-6);
    EXPECT_GT(sb.total(), 10.0);
}

TEST(RangeModel, PredictPreflopResponseShrinksWithSize) {
    const WeightedRange everything = WeightedRange::uniform();
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const Response standard = predict_preflop_response(everything, Position::BB, Position::BTN, 1, 1.0, 6, p);
    const Response big = predict_preflop_response(everything, Position::BB, Position::BTN, 1, 2.0, 6, p);
    EXPECT_NEAR(standard.fold + standard.call + standard.raise, 1.0, 1e-9);
    EXPECT_NEAR(big.fold + big.call + big.raise, 1.0, 1e-9);
    EXPECT_GT(big.fold, standard.fold);
    EXPECT_NEAR(big.raise, standard.raise, 1e-9);
    // The standard continue share is the chart's: raise + call ranges over all combos.
    const VsRaiseRanges& chart = vs_open_ranges(Position::BB, Position::BTN);
    int continuing = 0;
    for (const HandClass h : hand_classes_by_strength()) {
        if (chart.raise.contains(h) || chart.call.contains(h)) continuing += h.combos();
    }
    EXPECT_NEAR(1.0 - standard.fold, continuing / double(kComboCount), 1e-6);
    EXPECT_NEAR(standard.call_range.total() + standard.raise_range.total(), (1.0 - standard.fold) * kComboCount, 1e-3);
    // Facing a 3-bet the Nit continues less than the Maniac.
    const Response nit = predict_preflop_response(everything, Position::CO, Position::BTN, 2, 1.0, 6,
                                                  archetype_params(Archetype::Nit));
    const Response maniac = predict_preflop_response(everything, Position::CO, Position::BTN, 2, 1.0, 6,
                                                     archetype_params(Archetype::Maniac));
    EXPECT_GT(nit.fold, maniac.fold);
}

// --- Postflop inference ---------------------------------------------------------

// BTN opens, BB defends, both check to the river; the BB checks the river too.
// Returns the engine with the BTN to act on the river.
GameEngine checked_to_river_btn(const std::array<Card, 5>& board) {
    Holes holes{};
    holes[kBb] = hole("Qs", "Qd");
    GameEngine engine = table(holes, board);
    btn_open_bb_call(engine);
    play(engine, {X, X, X, X, X});
    EXPECT_EQ(engine.state().street, Street::River);
    return engine;
}

TEST(RangeModel, RiverBetShiftsWeightToStrongHands) {
    GameEngine engine = checked_to_river_btn(kDryBoard);
    const WeightedRange prior = infer_range_for(engine.player_view(kBb), kBtn, Archetype::Baseline, true);
    play(engine, {B(110)});  // Pot
    const WeightedRange after = infer_range_for(engine.player_view(kBb), kBtn, Archetype::Baseline, true);
    const StreetStrength s = street_strength(kDryBoard);
    EXPECT_NEAR(top_mass_share(prior, prior, s, 0.2), 0.2, 0.03);
    const double top = top_mass_share(after, prior, s, 0.2);
    std::printf("[ RangeModel ] top 20%% of the BTN's range: %.3f of its range before a pot bet, %.3f after\n",
                top_mass_share(prior, prior, s, 0.2), top);
    EXPECT_GT(top, 0.35);
    EXPECT_LT(after.total(), prior.total());
}

TEST(RangeModel, BluffShareTracksSize) {
    // What this checks, for a river bet of s pots by the Baseline:
    // 1. The inferred range is the prior times bet_likelihood (inference and the
    //    exposed likelihood agree), to float precision.
    // 2. bet_breakdown's bluff share of the polarised part, air / (air + value), is
    //    m*r / (1 + m*r) with r = s / (1 + s), m = bluff_mult: s / (1 + 2s) here.
    // 3. Measured in the inferred range itself, the mass on air combos (top_share
    //    > 0.6 in the prior) over that plus the value mass is within 0.05 of it; it
    //    differs only by the value edge's tail over air.
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const StreetStrength s = street_strength(kDryBoard);
    double previous = 0.0;
    for (const int chips : {36, 82, 138}) {  // 0.33, 0.75 and 1.25 pots of 110
        GameEngine engine = checked_to_river_btn(kDryBoard);
        const WeightedRange prior = infer_range_for(engine.player_view(kBb), kBtn, Archetype::Baseline, true);
        play(engine, {B(chips)});
        const WeightedRange after = infer_range_for(engine.player_view(kBb), kBtn, Archetype::Baseline, true);
        const double size = chips / 110.0;

        const BetBreakdown bet = bet_breakdown(prior, s, size, p);
        WeightedRange expected = prior;
        expected.multiply(bet.likelihood);
        for (int combo = 0; combo < kComboCount; ++combo) {
            ASSERT_NEAR(after.weight(combo), expected.weight(combo), 1e-6) << combo;
        }

        const double r = size / (1.0 + size);
        const double formula = p.bluff_mult * r / (1.0 + p.bluff_mult * r);
        ASSERT_LT(bet.bluff_weight, 1.0);
        EXPECT_NEAR(bet.air_share_of_polarised(), formula, 1e-6);

        const auto t = top_share(prior, s);
        double air = 0.0;
        for (int combo = 0; combo < kComboCount; ++combo) {
            if (t[static_cast<std::size_t>(combo)] > 0.6f) air += after.weight(combo);
        }
        const double measured = air / (air + bet.value_mass);
        std::printf("[ RangeModel ] bet %.2f pot: air share %.3f measured, %.3f formula s/(1+2s), bets %.0f%%\n",
                    size, measured, formula, 100.0 * bet.bet_share());
        EXPECT_NEAR(measured, formula, 0.05);
        EXPECT_GT(measured, previous);
        previous = measured;
    }
}

// The BB's defend range carried to a neutral river (no flush or straight
// possible) by checks, for an archetype.
WeightedRange bb_range_on_river(const ArchetypeParams& p, const std::array<Card, 5>& board) {
    Holes holes{};
    holes[kBtn] = hole("Qs", "Qd");
    GameEngine engine = table(holes, board);
    btn_open_bb_call(engine);
    play(engine, {X, X, X, X, X});
    return infer_range_for(engine.player_view(kBtn), kBb, p, true);
}

const std::array<Card, 5> kNeutralRiver = board_of("Kc", "8d", "3h", "2s", "Jh");

TEST(RangeModel, ContinueShareTracksMdfTimesMult) {
    const StreetStrength s = street_strength(kNeutralRiver);
    for (const Archetype a : kArchetypes) {
        ArchetypeParams p = archetype_params(a);
        const bool station = p.pairs_and_draws_continue;
        p.pairs_and_draws_continue = false;  // The formula without the Station's pair rule
        const WeightedRange range = bb_range_on_river(p, kNeutralRiver);
        for (const double size : {0.33, 0.5, 1.0, 2.0}) {
            const Response r = predict_postflop_response(range, s, Street::River, size, size / (1 + 2 * size), p);
            const double c = std::min(std::max(p.continue_mult / (1.0 + size), p.continue_floor), 0.95);
            EXPECT_NEAR(r.fold + r.call + r.raise, 1.0, 1e-9);
            EXPECT_NEAR(r.fold, 1.0 - c, 0.03) << to_string(a) << " at " << size;
            if (station) {
                const Response with_pairs = predict_postflop_response(range, s, Street::River, size,
                                                                      size / (1 + 2 * size), archetype_params(a));
                EXPECT_LE(with_pairs.fold, r.fold + 1e-9);
            }
        }
    }
}

// The raise share cap is raise_share x clamp(1.5 / (1 + s), 0.4, 1.25): fewer
// raises against bigger bets, the same cap in the predicted response and in the
// inference likelihood.
TEST(RangeModel, RaiseShareFallsAsBetSizeGrows) {
    EXPECT_NEAR(raise_share_scale(0.5), 1.0, 1e-12);
    EXPECT_NEAR(raise_share_scale(0.33), 1.5 / 1.33, 1e-12);
    EXPECT_NEAR(raise_share_scale(1.25), 1.5 / 2.25, 1e-12);
    EXPECT_NEAR(raise_share_scale(2.0), 0.5, 1e-12);
    EXPECT_NEAR(raise_share_scale(0.1), 1.25, 1e-12);
    EXPECT_NEAR(raise_share_scale(10.0), 0.4, 1e-12);

    const StreetStrength s = street_strength(kNeutralRiver);
    for (const Archetype a : kArchetypes) {
        const ArchetypeParams& p = archetype_params(a);
        const WeightedRange range = bb_range_on_river(p, kNeutralRiver);
        double previous = 1.0;
        for (const double size : {0.33, 0.5, 0.75, 1.25, 2.0}) {
            const Response r = predict_postflop_response(range, s, Street::River, size, size / (1 + 2 * size), p);
            const double cap = p.raise_share * raise_share_scale(size);
            EXPECT_LE(r.raise, cap + 1e-9) << to_string(a) << " at " << size;
            EXPECT_LT(r.raise, previous) << to_string(a) << " at " << size;
            previous = r.raise;

            // The likelihood a raise multiplies in has the same mean as the predicted share.
            const WeightedRange raise = raise_likelihood(range, s, size, p);
            double mass = 0.0;
            double raising = 0.0;
            for (int combo = 0; combo < kComboCount; ++combo) {
                mass += range.weight(combo);
                raising += range.weight(combo) * raise.weight(combo);
            }
            EXPECT_NEAR(raising / mass, r.raise, 1e-5) << to_string(a) << " at " << size;
        }
    }
    // The Baseline's cap binds from half pot up, so its share is the cap itself.
    const ArchetypeParams& base = archetype_params(Archetype::Baseline);
    const WeightedRange range = bb_range_on_river(base, kNeutralRiver);
    for (const double size : {0.5, 1.25, 2.0}) {
        const Response r = predict_postflop_response(range, s, Street::River, size, size / (1 + 2 * size), base);
        EXPECT_NEAR(r.raise, base.raise_share * raise_share_scale(size), 1e-6) << size;
    }
}

TEST(RangeModel, ArchetypesOrderAsTheirNamesSay) {
    // One range, the Baseline's flop defend range, answered by each type.
    Holes holes{};
    holes[kBtn] = hole("Qs", "Qd");
    GameEngine engine = table(holes, kDryBoard);
    btn_open_bb_call(engine);
    const WeightedRange range = *infer_ranges(engine.player_view(kBtn), Archetype::Baseline)[kBb];
    const std::array<Card, 3> flop{kDryBoard[0], kDryBoard[1], kDryBoard[2]};
    const StreetStrength s = street_strength(flop);

    for (const double size : {0.33, 0.75, 1.25}) {
        std::array<Response, kArchetypeCount> r;
        std::array<BetBreakdown, kArchetypeCount> bet;
        for (std::size_t k = 0; k < kArchetypeCount; ++k) {
            const ArchetypeParams& p = archetype_params(kArchetypes[k]);
            r[k] = predict_postflop_response(range, s, Street::Flop, size, size / (1 + 2 * size), p);
            bet[k] = bet_breakdown(range, s, size, p);
        }
        const auto [base, nit, station, maniac] = std::array<std::size_t, 4>{0, 1, 2, 3};
        EXPECT_GT(r[nit].fold, r[base].fold) << size;
        EXPECT_GT(r[base].fold, r[station].fold) << size;
        EXPECT_GT(r[base].fold, r[maniac].fold) << size;
        EXPECT_GT(r[maniac].fold, r[station].fold) << size;
        EXPECT_GT(r[maniac].raise, r[base].raise) << size;
        EXPECT_GT(r[base].raise, r[nit].raise) << size;
        for (std::size_t k = 1; k < 3; ++k) {
            EXPECT_GT(bet[maniac].bet_share(), bet[k].bet_share()) << size;
            EXPECT_GT(bet[maniac].air_mass, bet[k].air_mass) << size;
        }
        EXPECT_GT(bet[base].air_mass, bet[nit].air_mass) << size;
    }
}

TEST(RangeModel, UsersSpotIsPlausible) {
    // 8c8d on the BTN, opened and called by the BB, on Ah9s4c.
    const std::array<Card, 2> hero = hole("8c", "8d");
    Holes holes{};
    holes[kBtn] = hero;
    GameEngine engine = table(holes, board_of("Ah", "9s", "4c", "Kd", "2h"));
    btn_open_bb_call(engine);
    const PlayerView flop_view = engine.player_view(kBtn);
    const WeightedRange defend = *infer_ranges(flop_view, Archetype::Baseline)[kBb];
    const ComboEquities flop_eq = hero_vs_each_combo(hero, flop_view.board, combos_in(defend));
    const double before = equity_vs_range(flop_eq, defend).value();

    // The BB checks, hero bets a third, the BB calls: hero is worse against what is left.
    play(engine, {X, B(36), C});
    const PlayerView turn_view = engine.player_view(kBtn);
    const WeightedRange called = *infer_ranges(turn_view, Archetype::Baseline)[kBb];
    const double after = equity_vs_range(flop_eq, called).value();
    std::printf("[ RangeModel ] 88 on Ah9s4c: %.1f%% vs the BB defend range, %.1f%% vs its check-call range\n",
                100.0 * before, 100.0 * after);
    EXPECT_GE(before, 0.40);
    EXPECT_LE(before, 0.55);
    EXPECT_LT(after, before - 0.05);
    EXPECT_GT(after, 0.15);
}

// --- Hidden information and determinism -----------------------------------------

TEST(RangeModel, NeverUsesHiddenCards) {
    // Same public actions; the opponents' cards and the undealt turn and river differ.
    const auto ranges = [](const Holes& holes, const std::array<Card, 5>& board) {
        GameEngine engine = table(holes, board);
        play(engine, {F, F, R(50), C, F, C});  // CO opens, BTN and BB call
        play(engine, {X, B(50), C});           // Flop: BB checks, CO bets, BTN calls
        const PlayerView view = engine.player_view(kBb);
        EXPECT_EQ(view.to_act, kBb);
        return infer_ranges(view, Archetype::Station);
    };
    Holes a{};
    Holes b{};
    a[kBb] = b[kBb] = hole("Ts", "9s");
    a[kCo] = hole("Ac", "Ad");
    a[kBtn] = hole("7c", "2d");
    b[kCo] = hole("3c", "4d");
    b[kBtn] = hole("Kc", "Kd");
    const auto first = ranges(a, board_of("Qh", "8h", "2s", "Ah", "Jc"));
    const auto second = ranges(b, board_of("Qh", "8h", "2s", "6d", "5c"));
    for (std::size_t i = 0; i < kMaxSeats; ++i) {
        ASSERT_EQ(first[i].has_value(), second[i].has_value()) << i;
        if (first[i]) {
            EXPECT_EQ(first[i]->weights(), second[i]->weights()) << i;
        }
    }
    EXPECT_TRUE(first[kCo].has_value() && first[kBtn].has_value());
}

TEST(RangeModel, DeterministicAndCardRemovalApplied) {
    Holes holes{};
    holes[kBb] = hole("Ad", "Kc");
    GameEngine engine = table(holes, kDryBoard);
    btn_open_bb_call(engine);
    play(engine, {X, B(80), R(240), C});  // Flop check-raised and called
    play(engine, {B(300)});               // BB leads the turn
    const PlayerView view = engine.player_view(kBtn);
    // From the BTN's view, the BB's range; then from the BB's, its own and the BTN's.
    const PlayerView bb_view = engine.player_view(kBb);
    const auto one = infer_ranges(bb_view, Archetype::Maniac);
    const auto two = infer_ranges(bb_view, Archetype::Maniac);
    ASSERT_TRUE(one[kBtn].has_value());
    EXPECT_EQ(one[kBtn]->weights(), two[kBtn]->weights());

    const CardMask dead = dead_for(bb_view);
    for (int combo = 0; combo < kComboCount; ++combo) {
        if ((combo_mask(combo) & dead) != 0) {
            ASSERT_EQ(one[kBtn]->weight(combo), 0.0f) << combo;
        }
    }
    // Without removing the viewer's cards, the BTN keeps combos holding them...
    const WeightedRange kept = infer_range_for(bb_view, kBtn, Archetype::Maniac, false);
    double blocked = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if ((combo_mask(combo) & card_mask(std::span<const Card>(*bb_view.own_hole))) != 0) blocked += kept.weight(combo);
    }
    EXPECT_GT(blocked, 0.0);
    // ... and the viewer's own range never loses its own cards, only the board.
    const WeightedRange own = infer_range_for(bb_view, kBb, Archetype::Maniac, true);
    EXPECT_GT(own.weight(combo_index(card("Ad"), card("Kc"))), 0.0f);
    EXPECT_EQ(own.weight(combo_index(card("Kh"), card("Kd"))), 0.0f);  // Kh is on the board
    EXPECT_EQ(own.weights(), infer_range_for(view, kBb, Archetype::Maniac, false).weights());

    EXPECT_THROW(infer_range_for(view, kMaxSeats, Archetype::Baseline, true), std::invalid_argument);
}

// --- Reports (run with --gtest_also_run_disabled_tests) -------------------------

void print_range(const char* label, const WeightedRange& range, std::span<const Card> board) {
    const StreetStrength s = street_strength(board);
    const auto t = top_share(range, s);
    double strong = 0.0, pair = 0.0, draw = 0.0, air = 0.0;
    std::array<double, 3> by_share{};  // Top 33%, middle, weaker than 0.6
    for (int combo = 0; combo < kComboCount; ++combo) {
        const double w = range.weight(combo);
        if (w <= 0.0) continue;
        const auto i = static_cast<std::size_t>(combo);
        const HandRead read = read_hand(combo_cards(combo), board);
        if (*read.category <= HandCategory::TwoPair) {
            strong += w;
        } else if (*read.category == HandCategory::OnePair) {
            pair += w;
        } else if (read.strong_draw()) {
            draw += w;
        } else {
            air += w;
        }
        by_share[t[i] <= 0.33f ? 0 : t[i] <= 0.6f ? 1 : 2] += w;
    }
    const double total = range.total();
    std::vector<std::pair<double, HandClass>> classes;
    for (const HandClass h : hand_classes_by_strength()) {
        if (range.class_mass(h) > 0.0) classes.emplace_back(range.class_mass(h), h);
    }
    std::stable_sort(classes.begin(), classes.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::printf("  %-34s mass %6.1f (%4.1f%%) | 2pair+ %4.1f%% pair %4.1f%% draw %4.1f%% air %4.1f%%\n", label, total,
                100.0 * total / kComboCount, 100.0 * strong / total, 100.0 * pair / total, 100.0 * draw / total,
                100.0 * air / total);
    std::printf("  %-34s top: ", "");
    for (std::size_t k = 0; k < std::min<std::size_t>(10, classes.size()); ++k) {
        std::printf("%s %.1f  ", classes[k].second.to_string().c_str(), classes[k].first);
    }
    std::printf("\n");
}

void print_responses(const WeightedRange& range, std::span<const Card> board, Street street,
                     const ArchetypeParams& p) {
    const StreetStrength s = street_strength(board);
    std::printf("  %-34s", "  vs bet 33/75/125%: fold/call/raise");
    for (const double size : {0.33, 0.75, 1.25}) {
        const Response r = predict_postflop_response(range, s, street, size, size / (1 + 2 * size), p);
        std::printf("  %2.0f/%2.0f/%2.0f", 100 * r.fold, 100 * r.call, 100 * r.raise);
    }
    std::printf("\n");
}

void report(const char* title, GameEngine& engine, int viewer, int villain) {
    const PlayerView view = engine.player_view(viewer);
    std::printf("\n%s  board:", title);
    for (const Card c : view.board) std::printf(" %s", c.to_string().c_str());
    std::printf("  viewer %s%s\n", (*view.own_hole)[0].to_string().c_str(), (*view.own_hole)[1].to_string().c_str());
    for (const Archetype a : kArchetypes) {
        const ArchetypeParams& p = archetype_params(a);
        const WeightedRange r = *infer_ranges(view, p)[static_cast<std::size_t>(villain)];
        print_range(std::string(to_string(a)).c_str(), r, view.board);
        print_responses(r, view.board, view.street, p);
        const ComboEquities eq = hero_vs_each_combo(*view.own_hole, view.board, combos_in(r));
        std::printf("  %-34s  viewer's equity vs this range %.1f%%\n", "", 100.0 * equity_vs_range(eq, r).value());
    }
}

TEST(RangeModelReport, DISABLED_ThreeSpots) {
    {
        Holes holes{};
        holes[kBtn] = hole("As", "Qd");
        GameEngine engine = table(holes, board_of("Kh", "7d", "2c", "5s", "Jh"));
        btn_open_bb_call(engine);
        play(engine, {X, B(36), C, X});  // Flop c-bet a third, called; BB checks the turn
        report("1. BTN vs BB single-raised, K72r c-bet/call, turn 5s checked to the BTN (BB's range)", engine, kBtn,
               kBb);
    }
    {
        Holes holes{};
        holes[kBb] = hole("Kc", "9c");
        GameEngine engine = table(holes, board_of("Qs", "9h", "4d", "2c", "7s"));
        btn_open_bb_call(engine);
        play(engine, {X, B(55), C, X, B(165), C, X, B(330)});  // 50%, 75%, 75% pot
        report("2a. Bet-bet-bet: the BTN's river betting range (BB facing 75%)", engine, kBb, kBtn);
    }
    {
        Holes holes{};
        holes[kBtn] = hole("As", "Qd");
        GameEngine engine = table(holes, board_of("Qs", "9h", "4d", "2c", "7s"));
        btn_open_bb_call(engine);
        play(engine, {X, B(55), C, X, B(165), C, X});
        report("2b. Bet-bet, river checked: the BB's check-call-check-call-check range", engine, kBtn, kBb);
    }
    {
        Holes holes{};
        holes[kBtn] = hole("Ah", "Kc");
        GameEngine engine = table(holes, board_of("Js", "8s", "3d", "Kd", "2h"));
        play(engine, {F, F, R(50), R(160), F, F, C, X});  // CO opens, BTN 3-bets, CO calls and checks
        report("3. 3-bet pot: CO calls the BTN's 3-bet, checks J83 two-tone (CO's range)", engine, kBtn, kCo);
    }
}

TEST(RangeModelReport, DISABLED_Timing) {
    for (const std::size_t cards : {3u, 4u, 5u}) {
        const std::array<Card, 5> board = board_of("Qs", "9h", "4d", "2c", "7s");
        constexpr int kRuns = 200;
        std::vector<double> ms;
        for (int run = 0; run < kRuns; ++run) {
            const auto start = std::chrono::steady_clock::now();
            const StreetStrength s = street_strength(std::span<const Card>(board.data(), cards));
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            ASSERT_GT(s.live_count, 0);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ RangeModel ] street_strength, %zu cards: p50 %.3f ms, p95 %.3f ms\n", cards, ms[kRuns / 2],
                    ms[kRuns * 95 / 100]);
    }
    // River views with 2 to 5 opponents, bets and calls on every street.
    for (int opponents = 2; opponents <= 5; ++opponents) {
        GameEngine engine = table({}, board_of("Qs", "9h", "4d", "2c", "7s"));
        // UTG opens; the next players call or fold so `opponents` + 1 see the flop; BB is hero.
        std::vector<Action> pre{R(50)};
        int callers = 0;
        for (int k = 0; k < 4; ++k) {
            const bool call = callers < opponents - 1;
            pre.push_back(call ? C : F);
            callers += call ? 1 : 0;
        }
        pre.push_back(C);  // BB
        for (const Action& a : pre) ASSERT_TRUE(engine.apply_action(a));
        int hero = -1;
        while (engine.state().street != Street::River) {
            const int to_act = *engine.state().to_act;
            const PlayerView v = engine.player_view(to_act);
            if (hero < 0) hero = to_act;
            const Action a = v.legal.allows(ActionType::Bet) && to_act != hero ? B(v.total_pot / 2)
                             : v.legal.allows(ActionType::Call)                  ? C
                                                                                 : X;
            ASSERT_TRUE(engine.apply_action(a));
        }
        const PlayerView view = engine.player_view(*engine.state().to_act);
        constexpr int kRuns = 200;
        std::vector<double> ms;
        for (int run = 0; run < kRuns; ++run) {
            const auto start = std::chrono::steady_clock::now();
            const auto ranges = infer_ranges(view, Archetype::Baseline);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            ASSERT_EQ(std::count_if(ranges.begin(), ranges.end(), [](const auto& r) { return r.has_value(); }),
                      view.opponents_in_hand());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ RangeModel ] river, %d opponents, %zu actions: infer_ranges p50 %.2f ms, p95 %.2f ms\n",
                    view.opponents_in_hand(), view.actions.size(), ms[kRuns / 2], ms[kRuns * 95 / 100]);
    }
}

}  // namespace
