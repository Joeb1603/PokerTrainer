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
#include "strategy/push_fold.h"

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

    // The BTN is the viewer here only for card removal's sake: use no removal.
    const WeightedRange co = infer_range_for(view, kCo, Archetype::Baseline, false);
    const WeightedRange btn = infer_range_for(engine.player_view(kSb), kBtn, Archetype::Baseline, false);
    const VsRaiseRanges& vs_co = vs_open_ranges(Position::BTN, Position::CO);
    // The CO opened and faces the BTN's 3-bet out of position: the positional table.
    const VsRaiseRanges& co_vs_btn = vs_three_bet_ranges(Position::CO, Position::BTN);
    for (const HandClass h : hand_classes_by_strength()) {
        const double b = btn.class_mass(h) / h.combos();
        EXPECT_NEAR(b, vs_co.raise.contains(h) ? 1.0 - p.slowplay : 0.0, 1e-6) << h.to_string();
        const double c = co.class_mass(h) / h.combos();
        const bool four_bets = co_vs_btn.raise.contains(h) && rfi_range(Position::CO).contains(h);
        EXPECT_NEAR(c, four_bets ? 1.0 - p.slowplay : 0.0, 1e-6) << h.to_string();
    }

    // Calling the 4-bet as the 3-bettor: the positional vs-4-bet call range plus
    // slowplayed 5-bet hands, on the BTN's 3-bet range.
    play(engine, {C});
    ASSERT_EQ(engine.state().street, Street::Flop);
    const WeightedRange caller = infer_range_for(engine.player_view(kCo), kBtn, Archetype::Baseline, false);
    const VsRaiseRanges& btn_vs_co = vs_four_bet_ranges(Position::BTN, Position::CO);
    WeightedRange expected;
    for (const HandClass h : hand_classes_by_strength()) {
        const double three_bet = vs_co.raise.contains(h) ? 1.0 - p.slowplay : 0.0;
        const double flat = btn_vs_co.raise.contains(h) ? p.slowplay : btn_vs_co.call.contains(h) ? 1.0 : 0.0;
        expected.set_class(h, static_cast<float>(three_bet * flat));
    }
    expected.remove_cards(card_mask(std::span<const Card>(engine.player_view(kCo).board)));
    for (int combo = 0; combo < kComboCount; ++combo) {
        EXPECT_NEAR(caller.weight(combo), expected.weight(combo), 1e-6) << combo;
    }
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
    const CardMask board = card_mask(std::span<const Card>(kDryBoard));  // The view's board
    EXPECT_NEAR(at10, chart_weights(*push_fold_jam_range(Position::BTN, 10), 0).total(), 1e-6);  // The 10 BB table

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
    EXPECT_NEAR(caller.total(), chart_weights(HandRange::parse("JJ+, AKs, AKo"), board).total(), 1e-6);
    GameEngine shallow = table({}, kDryBoard, {200, 2000, 2000, 2000, 2000, 2000});
    play(shallow, {F, F, F, R(200), F, C});
    const WeightedRange short_call = infer_range_for(shallow.player_view(kBtn), kBb, Archetype::Baseline, false);
    // The BB's push/fold call range against a 10 BB BTN jam.
    EXPECT_EQ(short_call.weights(), chart_weights(*push_fold_call_range(Position::BB, Position::BTN, 10), board).weights());
    EXPECT_GT(short_call.total(), caller.total());
}

TEST(RangeModel, ShortAllInUsesPushFoldTables) {
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const auto shove = [](int stack, std::initializer_list<Action> before) {
        GameEngine engine = table({}, kDryBoard, {stack, 2000, 2000, 2000, 2000, 2000});
        play(engine, before);
        play(engine, {R(stack)});  // The BTN goes all in
        return infer_range_for(engine.player_view(kSb), kBtn, Archetype::Baseline, false);
    };
    // First in: the jam table at each stack, and no cliff past 15 BB: from 15 to 25
    // BB the range blends into the commit range by (D - 15) / 10.
    const HandRange commit = HandRange::parse("JJ+, AKs, AKo");
    const HandRange jam15 = *push_fold_jam_range(Position::BTN, 15);
    for (const int stack : {60, 100, 200, 300}) {
        const WeightedRange r = shove(stack, {F, F, F});
        EXPECT_EQ(r.weights(), chart_weights(*push_fold_jam_range(Position::BTN, stack / 20), 0).weights()) << stack;
    }
    double previous = shove(300, {F, F, F}).total();
    for (const int stack : {320, 340, 400, 500}) {
        const WeightedRange r = shove(stack, {F, F, F});
        const double t = (stack / 20.0 - 15.0) / 10.0;
        for (const HandClass h : hand_classes_by_strength()) {
            const double expected = (jam15.contains(h) ? 1.0 - t : 0.0) + (commit.contains(h) ? t : 0.0);
            ASSERT_NEAR(r.class_mass(h) / h.combos(), expected, 1e-6) << stack << " " << h.to_string();
        }
        EXPECT_LT(r.total(), previous) << stack;
        previous = r.total();
    }
    const double at15 = shove(300, {F, F, F}).total();
    const double at16 = shove(320, {F, F, F}).total();
    EXPECT_GT(at16, 0.85 * at15);  // Was the top 27% at 15 BB and the commit range (3%) at 16
    EXPECT_NEAR(shove(500, {F, F, F}).total(), commit.combos(), 1e-6);
    EXPECT_NEAR(shove(2000, {F, F, F}).total(), commit.combos(), 1e-6);

    // A re-jam over the CO's open: what calls a 10 BB jam from the CO.
    const WeightedRange rejam = shove(200, {F, F, R(50)});
    const WeightedRange expected = chart_weights(resize_range(*push_fold_call_range(Position::BTN, Position::CO, 10),
                                                              p.three_bet_width), 0);
    EXPECT_EQ(rejam.weights(), expected.weights());

    // The SB's call of the BTN's 10 BB jam, and the BB's overcall: the call table.
    GameEngine engine = table({}, kDryBoard, {200, 2000, 2000, 2000, 2000, 2000});
    play(engine, {F, F, F, R(200), C, C});
    const PlayerView view = engine.player_view(kBtn);
    const CardMask board = card_mask(std::span<const Card>(view.board));
    EXPECT_EQ(infer_range_for(view, kSb, Archetype::Baseline, false).weights(),
              chart_weights(*push_fold_call_range(Position::SB, Position::BTN, 10), board).weights());
    EXPECT_EQ(infer_range_for(view, kBb, Archetype::Baseline, false).weights(),
              chart_weights(*push_fold_call_range(Position::BB, Position::BTN, 10), board).weights());
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

TEST(RangeModel, SmallRaisesGetCalledMore) {
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    // The BB against a BTN open of 1.5, 2 and 2.5 BB (standard 2.5): more calls as the
    // open shrinks, the same 3-bets. The extra calls come from hands outside the chart.
    const WeightedRange everything = WeightedRange::uniform();
    const VsRaiseRanges& bb_chart = vs_open_ranges(Position::BB, Position::BTN);
    const Response r06 = predict_preflop_response(everything, Position::BB, Position::BTN, 1, 0.6, 6, p);
    const Response r08 = predict_preflop_response(everything, Position::BB, Position::BTN, 1, 0.8, 6, p);
    const Response r10 = predict_preflop_response(everything, Position::BB, Position::BTN, 1, 1.0, 6, p);
    EXPECT_GT(r06.call, r08.call + 0.03);
    EXPECT_GT(r08.call, r10.call + 0.03);
    EXPECT_NEAR(r06.raise, r10.raise, 1e-9);
    bool outside = false;
    for (const HandClass h : hand_classes_by_strength()) {
        if (bb_chart.call.contains(h) || bb_chart.raise.contains(h)) continue;
        outside = outside || r08.call_range.class_mass(h) > 0.0;
        EXPECT_EQ(r10.call_range.class_mass(h), 0.0) << h.to_string();
    }
    EXPECT_TRUE(outside);

    // The CO's open facing the BB's 3-bet: a min-3-bet (to 80 over 50, 0.53 of the
    // standard 150) is called far more than a standard one. Before Stage 6 the call
    // share was flat from 0.55 to 1.0, because the call chart grew only through hands
    // already in the raise chart.
    const WeightedRange open = WeightedRange::from_hand_range(rfi_range(Position::CO));
    PreflopSpot spot;
    spot.villain = Position::CO;
    spot.raiser = Position::BB;
    spot.raises = 2;
    spot.size_ratio = 80.0 / 150.0;
    const Response min3 = predict_preflop_response(open, spot, p);
    spot.size_ratio = 1.0;
    const Response standard = predict_preflop_response(open, spot, p);
    spot.size_ratio = 1.4;
    const Response big = predict_preflop_response(open, spot, p);
    EXPECT_GT(min3.call, standard.call + 0.15);
    EXPECT_GT(standard.call, big.call);
    EXPECT_NEAR(min3.raise, standard.raise, 1e-9);
    // The standard answer is the positional table: about 45% of the CO's open continues.
    EXPECT_NEAR(1.0 - standard.fold, 168.0 / 370.0, 0.01);
    std::printf("[ RangeModel ] BB calls a BTN open of 1.5/2/2.5 BB %.1f/%.1f/%.1f%%; the CO calls a BB 3-bet "
                "of 0.53/1/1.4x %.1f/%.1f/%.1f%%\n",
                100 * r06.call, 100 * r08.call, 100 * r10.call, 100 * min3.call, 100 * standard.call, 100 * big.call);
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
    // What this checks, for a river lead of s pots by the Baseline BB after the
    // flop and turn were checked through:
    // 1. The inferred range is the prior times bet_likelihood against the BTN's
    //    range (inference and the exposed likelihood agree), to float precision.
    //    The viewer is a player who folded, with no card removal, so the BTN's
    //    range given here is the one the walk itself holds.
    // 2. bet_breakdown's bluff share of the polarised part, air / (air + value), is
    //    m*r / (1 + m*r) with r = s / (1 + s), m = bluff_mult: s / (1 + 2s) here.
    // 3. Measured in the inferred range itself, the mass on air combos (top_share
    //    > 0.6 in the BTN's range) over that plus the value mass is within 0.05 of
    //    it; it differs only by the value edge reaching into air.
    // (The BTN's own river bets are a poor case: as the flop's aggressor it bluffed
    // most of its air, so its checked range holds too little air to calibrate.)
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const StreetStrength s = street_strength(kDryBoard);
    double previous = 0.0;
    for (const int chips : {36, 82, 138}) {  // 0.33, 0.75 and 1.25 pots of 110
        GameEngine engine = table({}, kDryBoard);
        btn_open_bb_call(engine);
        play(engine, {X, X, X, X});
        ASSERT_EQ(engine.state().street, Street::River);
        const PlayerView before = engine.player_view(kUtg);
        const WeightedRange prior = infer_range_for(before, kBb, Archetype::Baseline, false);
        const WeightedRange btn = infer_range_for(before, kBtn, Archetype::Baseline, false);
        play(engine, {B(chips)});
        const WeightedRange after = infer_range_for(engine.player_view(kUtg), kBb, Archetype::Baseline, false);
        const double size = chips / 110.0;

        const BetBreakdown bet = bet_breakdown(prior, s, size, p, &btn);
        WeightedRange expected = prior;
        expected.multiply(bet.likelihood);
        for (int combo = 0; combo < kComboCount; ++combo) {
            ASSERT_NEAR(after.weight(combo), expected.weight(combo), 1e-6) << combo;
        }
        EXPECT_EQ(bet_likelihood(prior, s, size, p, &btn).weights(), bet.likelihood.weights());

        const double r = size / (1.0 + size);
        const double formula = p.bluff_mult * r / (1.0 + p.bluff_mult * r);
        ASSERT_LT(bet.bluff_weight, 0.9);  // Below the bluff weight cap
        EXPECT_NEAR(bet.air_share_of_polarised(), formula, 1e-6);

        const auto u = top_share(btn, s);
        double air = 0.0;
        for (int combo = 0; combo < kComboCount; ++combo) {
            if (u[static_cast<std::size_t>(combo)] > 0.6f) air += after.weight(combo);
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

// --- Review fixes: value against the opponents, limps, fallback, widths -------

const std::array<Card, 5> kQ94 = board_of("Qs", "9h", "4d", "2c", "7s");

// AcQc on the BTN, opened and called by the BB on Qs9h4d 2c 7s: AQ's equity
// against the BB's range once `line` (after the preflop actions) is played.
double aq_equity_vs_bb(std::initializer_list<Action> line, Archetype a) {
    Holes holes{};
    holes[kBtn] = hole("Ac", "Qc");
    GameEngine engine = table(holes, kQ94);
    btn_open_bb_call(engine);
    play(engine, line);
    const PlayerView view = engine.player_view(kBtn);
    const WeightedRange r = *infer_ranges(view, a)[kBb];
    return equity_vs_range(hero_vs_each_combo(*view.own_hole, view.board, combos_in(r)), r).value();
}

TEST(RangeModel, CheckRaiseRangesAreStrong) {
    // A raise is value only if it beats enough of the bettor's range, so a
    // check-raise after passive play is mostly strong hands, not middle pairs.
    const double flop = aq_equity_vs_bb({X, B(36), R(150)}, Archetype::Baseline);
    const double river = aq_equity_vs_bb({X, B(36), C, X, X, X, B(137), R(480)}, Archetype::Baseline);
    const double river_after_calls =
        aq_equity_vs_bb({X, B(36), C, X, B(120), C, X, B(225), R(700)}, Archetype::Baseline);
    const double lead = aq_equity_vs_bb({X, B(36), C, X, X, B(137)}, Archetype::Baseline);
    std::printf("[ RangeModel ] AQ vs BB: flop x/r %.1f%%, river x/r %.1f%% and %.1f%%, river lead %.1f%%\n",
                100.0 * flop, 100.0 * river, 100.0 * river_after_calls, 100.0 * lead);
    EXPECT_GE(flop, 0.45);
    EXPECT_LE(flop, 0.60);
    EXPECT_LE(river, 0.40);
    EXPECT_LE(river_after_calls, 0.40);
    EXPECT_LT(lead, 0.80);  // Plausible is about 55-65%; the model gives about 73% (logged)
    // The Nit's raises are stronger still, the Maniac's weaker.
    EXPECT_LT(aq_equity_vs_bb({X, B(36), R(150)}, Archetype::Nit), flop);
    EXPECT_GT(aq_equity_vs_bb({X, B(36), R(150)}, Archetype::Maniac), flop);
}

// The share of `bettor`'s range that bets `chips` after `line`, read from the
// inference: its mass after the bet over its mass before.
double bet_frequency(const std::array<Card, 5>& board, std::initializer_list<Action> line, Action bet, int bettor,
                     int viewer) {
    GameEngine engine = table({}, board);
    play(engine, line);
    const double before = infer_range_for(engine.player_view(viewer), bettor, Archetype::Baseline, false).total();
    play(engine, {bet});
    return infer_range_for(engine.player_view(viewer), bettor, Archetype::Baseline, false).total() / before;
}

TEST(RangeModel, CbetFrequenciesArePlausible) {
    // The BB's 3-bet range (QQ+, AK, A5s) against the UTG's flat on K83r. The UTG's
    // positional flat (QQ-88, AKo, AQs-ATs, KQs-KTs and suited connectors) holds
    // AKo and Kx, so the BB bets a third of the pot most of the time, not always.
    const double three_bet_pot =
        bet_frequency(board_of("Kd", "8s", "3c", "2h", "Th"), {R(50), F, F, F, F, R(200), C}, B(136), kBb, kBtn);
    // The BTN against the BB's defend on K72r, a third of the pot.
    const double single_raised = bet_frequency(kDryBoard, {F, F, F, R(50), F, C, X}, B(36), kBtn, kUtg);
    std::printf("[ RangeModel ] c-bet a third: 3-bet pot K83r %.0f%%, BTN vs BB K72r %.0f%%\n", 100.0 * three_bet_pot,
                100.0 * single_raised);
    EXPECT_GE(three_bet_pot, 0.70);
    EXPECT_GE(single_raised, 0.60);
    EXPECT_LE(single_raised, 0.80);
}

TEST(RangeModel, PredictedRaisesAreJudgedAgainstTheBettor) {
    // The same river range facing a pot bet from a strong range and from a weak
    // one: its value raises are fewer and stronger against the strong one.
    const StreetStrength s = street_strength(kNeutralRiver);
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const WeightedRange range = bb_range_on_river(p, kNeutralRiver);
    WeightedRange strong;
    WeightedRange weak;
    const auto order = s.live_order();
    for (std::size_t k = 0; k < order.size(); ++k) {
        (k < order.size() / 10 ? strong : weak).set(order[k], 1.0f);
    }
    const auto mean_hs = [&](const WeightedRange& r) {
        double sum = 0.0;
        for (int combo = 0; combo < kComboCount; ++combo) sum += r.weight(combo) * s.hs[static_cast<std::size_t>(combo)];
        return sum / r.total();
    };
    const Response vs_strong = predict_postflop_response(range, s, Street::River, 1.0, 1.0 / 3.0, p, &strong);
    const Response vs_weak = predict_postflop_response(range, s, Street::River, 1.0, 1.0 / 3.0, p, &weak);
    const Response own_rule = predict_postflop_response(range, s, Street::River, 1.0, 1.0 / 3.0, p);
    EXPECT_NEAR(vs_strong.fold + vs_strong.call + vs_strong.raise, 1.0, 1e-9);
    EXPECT_LT(vs_strong.raise, vs_weak.raise);
    EXPECT_GT(mean_hs(vs_strong.raise_range), mean_hs(vs_weak.raise_range));
    EXPECT_LE(own_rule.raise, p.raise_share * raise_share_scale(1.0) + 1e-9);
    // The exposed likelihood matches the prediction for the same bettor.
    const WeightedRange raise = raise_likelihood(range, s, 1.0, p, &strong);
    double raising = 0.0;
    for (int combo = 0; combo < kComboCount; ++combo) raising += range.weight(combo) * raise.weight(combo);
    EXPECT_NEAR(raising / range.total(), vs_strong.raise, 1e-5);
}

TEST(RangeModel, LimpCallKeepsASensibleRange) {
    // UTG limps, the BTN iso-raises, the UTG calls or re-raises.
    for (const Archetype a : kArchetypes) {
        GameEngine limp = table({}, kDryBoard);
        play(limp, {C, F, F, R(100), F, F});
        const double limped = infer_range_for(limp.player_view(kBtn), kUtg, a, false).total();
        GameEngine call = limp;
        play(call, {C});
        const WeightedRange called = infer_range_for(call.player_view(kBtn), kUtg, a, false);
        GameEngine raise = limp;
        play(raise, {R(300)});
        const WeightedRange raised = infer_range_for(raise.player_view(kBtn), kUtg, a, false);
        std::printf("[ RangeModel ] %s UTG limp %.1f combos, limp-call %.1f, limp-raise %.1f\n",
                    std::string(to_string(a)).c_str(), limped, called.total(), raised.total());
        // The call keeps the top of the limp window (0.6 x flat_width of it) and
        // some traps: a real share of the limps, not the old handful of combos.
        EXPECT_GT(called.total(), 8.0) << to_string(a);
        EXPECT_GT(called.total(), 0.25 * limped) << to_string(a);
        EXPECT_LT(called.total(), 0.9 * limped) << to_string(a);
        EXPECT_EQ(class_weight(called, "72o"), 0.0) << to_string(a);
        // The re-raise holds the premiums that limped as traps, at 1 - sp of
        // their limp weight, and for the tighter types is mostly them.
        const ArchetypeParams& p = archetype_params(a);
        EXPECT_NEAR(class_weight(raised, "AA"), p.limp_trap * (1.0 - p.slowplay), 1e-6) << to_string(a);
        EXPECT_EQ(class_weight(raised, "72o"), 0.0) << to_string(a);
        double premium = 0.0;
        for (const HandClass h : hand_classes_by_strength()) {
            if (preflop_tier(h) == PreflopTier::Premium) premium += raised.class_mass(h);
        }
        if (a != Archetype::Maniac) {
            EXPECT_GT(premium, 0.6 * raised.total()) << to_string(a);
        }
    }
    // The SB completes, the BB raises, the SB calls: the same rule.
    GameEngine sb = table({}, kDryBoard);
    play(sb, {F, F, F, F, C, R(80), C});
    EXPECT_GT(infer_range_for(sb.player_view(kBb), kSb, Archetype::Baseline, false).total(), 5.0);
    // The prediction for a limper facing an iso-raise uses the same rule.
    const WeightedRange limp_range = infer_range_for(
        [] {
            GameEngine e = table({}, kDryBoard);
            play(e, {C, F, F});
            return e.player_view(kBtn);
        }(),
        kUtg, Archetype::Baseline, false);
    const ArchetypeParams& p = archetype_params(Archetype::Baseline);
    const Response as_limper = predict_preflop_response(limp_range, Position::UTG, Position::BTN, 1, 1.0, 6, p, true);
    const Response as_opener = predict_preflop_response(limp_range, Position::UTG, Position::BTN, 1, 1.0, 6, p);
    EXPECT_GT(as_limper.call, 2.0 * as_opener.call);
    EXPECT_NEAR(as_limper.fold + as_limper.call + as_limper.raise, 1.0, 1e-9);
}

TEST(RangeModel, FallbackNeverRevivesPreflopFolds) {
    // A Nit's river check-raise after calling two barrels: its range holds almost
    // nothing that raises, yet hands it folded preflop must stay out.
    for (const Archetype a : {Archetype::Nit, Archetype::Station}) {
        GameEngine engine = table({}, kQ94);
        btn_open_bb_call(engine);
        play(engine, {X, B(36), C, X, B(120), C, X, B(225), R(700)});
        const WeightedRange r = infer_range_for(engine.player_view(kUtg), kBb, a, false);
        EXPECT_GT(r.total(), 0.0) << to_string(a);
        EXPECT_EQ(class_weight(r, "72o"), 0.0) << to_string(a);
        EXPECT_EQ(class_weight(r, "83o"), 0.0) << to_string(a);
    }
    // Force the fallback: raises so rare that every product is untrusted. The
    // likelihood then lands on the preflop range, never on every combo.
    ArchetypeParams rare = archetype_params(Archetype::Baseline);
    rare.raise_share = 0.002;
    GameEngine engine = table({}, kQ94);
    btn_open_bb_call(engine);
    const WeightedRange defend = infer_range_for(engine.player_view(kUtg), kBb, rare, false);
    play(engine, {X, B(36), R(150)});
    const WeightedRange raised = infer_range_for(engine.player_view(kUtg), kBb, rare, false);
    EXPECT_GT(raised.total(), 0.0);
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (defend.weight(combo) == 0.0f) {
            ASSERT_EQ(raised.weight(combo), 0.0f) << combo;
        }
    }
}

TEST(RangeModel, WidthsOrderByTypeHeadsUpAndByPosition) {
    // The cap limits growth only and never cuts below the chart, so heads-up,
    // where the BTN chart is 75%, the loose types still open wider than it.
    const auto hu_open = [](Archetype a) {
        GameEngine engine = table({}, kDryBoard, {2000, 0, 2000, 0, 0, 0});
        play(engine, {R(50)});  // The BTN (small blind) opens
        return infer_range_for(engine.player_view(kBb), kBtn, a, false).total();
    };
    const double base = hu_open(Archetype::Baseline);
    EXPECT_NEAR(base, rfi_range(Position::BTN, 2).combos(), 1e-6);
    EXPECT_LT(hu_open(Archetype::Nit), base);
    EXPECT_GT(hu_open(Archetype::Station), base);
    EXPECT_GT(hu_open(Archetype::Maniac), base);
    // The Nit is narrower than the chart by its width alone, so its opens still
    // widen with position.
    double previous = 0.0;
    for (const int opener : {kUtg, kHj, kCo, kBtn}) {
        GameEngine engine = table({}, kDryBoard);
        while (*engine.state().to_act != opener) play(engine, {F});
        play(engine, {R(50)});
        const WeightedRange r = infer_range_for(engine.player_view(kBb), opener, Archetype::Nit, false);
        const HandRange& chart = rfi_range(*engine.player_view(kBb).seats[static_cast<std::size_t>(opener)].position);
        EXPECT_GT(r.total(), previous) << opener;
        EXPECT_NEAR(r.total() / chart.combos(), 0.6, 0.05) << opener;
        previous = r.total();
    }
}

TEST(RangeModel, ShortAllInIsNotANewRaiseLevel) {
    // The CO opens to 50 and the BTN goes all in for 70, 20 more: not a full
    // raise (30). The SB's raise to 200 is a 3-bet, as if the BTN had folded.
    const auto sb_range = [](bool short_all_in) {
        GameEngine engine = table({}, kDryBoard, {70, 2000, 2000, 2000, 2000, 2000});
        play(engine, {F, F, R(50)});
        play(engine, {short_all_in ? R(70) : F});
        play(engine, {R(200)});
        return infer_range_for(engine.player_view(kBb), kSb, Archetype::Baseline, false);
    };
    const WeightedRange after_short = sb_range(true);
    expect_same(after_short, sb_range(false));
    EXPECT_GT(after_short.total(), 0.0);
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
