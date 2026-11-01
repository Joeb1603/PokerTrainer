#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>

#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

HandClass hc(const char* text) { return HandClass::parse(text).value(); }

constexpr std::array<Position, 5> kOpeners{Position::UTG, Position::HJ, Position::CO, Position::BTN, Position::SB};

HandRange defended(const VsRaiseRanges& ranges) {
    HandRange all = ranges.call;
    for (const HandClass hand : hand_classes_by_strength()) {
        if (ranges.raise.contains(hand)) all.add(hand);
    }
    return all;
}

}  // namespace

// --- Hand classes ------------------------------------------------------------

TEST(HandClass, ParsesAndPrintsAllForms) {
    for (const char* text : {"AA", "22", "AKs", "AKo", "T9s", "72o"}) {
        EXPECT_EQ(hc(text).to_string(), text);
    }
    EXPECT_EQ(hc("KAs"), hc("AKs"));
}

TEST(HandClass, RejectsMalformedText) {
    for (const char* text : {"", "A", "AK", "AAs", "AAo", "AKx", "1Ks", "AKss", "aks"}) {
        EXPECT_FALSE(HandClass::parse(text).has_value()) << text;
    }
}

TEST(HandClass, FromCardsNormalisesOrderAndSuitedness) {
    const auto of = [](const char* a, const char* b) {
        return HandClass::from_cards(Card::parse(a).value(), Card::parse(b).value());
    };
    EXPECT_EQ(of("As", "Ks"), hc("AKs"));
    EXPECT_EQ(of("Ks", "As"), hc("AKs"));
    EXPECT_EQ(of("Kd", "As"), hc("AKo"));
    EXPECT_EQ(of("Th", "Td"), hc("TT"));
}

TEST(HandClass, EveryPairOfCardsMapsToAClassWithTheRightComboCounts) {
    std::array<int, kHandClassCount> counted{};
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            ++counted[static_cast<std::size_t>(HandClass::from_cards(Card(a), Card(b)).index())];
        }
    }
    int total = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        EXPECT_EQ(counted[static_cast<std::size_t>(hand.index())], hand.combos()) << hand.to_string();
        total += hand.combos();
    }
    EXPECT_EQ(total, kTotalCombos);
}

// --- Strength order ----------------------------------------------------------

TEST(StrengthOrder, Holds169UniqueClassesTotalling1326Combos) {
    std::set<std::string> names;
    std::set<int> indices;
    int pairs = 0;
    int suited = 0;
    int offsuit = 0;
    int combos = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        names.insert(hand.to_string());
        indices.insert(hand.index());
        EXPECT_GE(hand.index(), 0);
        EXPECT_LT(hand.index(), kHandClassCount);
        combos += hand.combos();
        if (hand.is_pair()) ++pairs;
        else if (hand.suited) ++suited;
        else ++offsuit;
    }
    EXPECT_EQ(names.size(), 169u);
    EXPECT_EQ(indices.size(), 169u);
    EXPECT_EQ(pairs, 13);
    EXPECT_EQ(suited, 78);
    EXPECT_EQ(offsuit, 78);
    EXPECT_EQ(combos, 1326);
}

TEST(StrengthOrder, PercentilesRiseFromAcesToTheBottom) {
    const auto& order = hand_classes_by_strength();
    EXPECT_EQ(order.front(), hc("AA"));
    EXPECT_EQ(strength_rank(hc("AA")), 0);
    EXPECT_NEAR(strength_percentile(hc("AA")), 100.0 * 6 / 1326, 1e-9);
    EXPECT_NEAR(strength_percentile(order.back()), 100.0, 1e-9);

    double previous = 0.0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(strength_rank(order[i]), static_cast<int>(i));
        EXPECT_GT(strength_percentile(order[i]), previous);
        previous = strength_percentile(order[i]);
    }
}

TEST(StrengthOrder, KnownHandsLandWhereExpected) {
    EXPECT_GT(strength_percentile(hc("72o")), 97.0);
    EXPECT_LT(strength_percentile(hc("AKs")), 3.0);
    EXPECT_LT(strength_rank(hc("AKs")), strength_rank(hc("AKo")));
    EXPECT_LT(strength_rank(hc("KQs")), strength_rank(hc("KQo")));
    EXPECT_LT(strength_rank(hc("TT")), strength_rank(hc("55")));
    EXPECT_LT(strength_rank(hc("JTs")), strength_rank(hc("A9o")));
    EXPECT_LT(strength_rank(hc("76s")), strength_rank(hc("76o")));
}

TEST(StrengthOrder, EverySuitedHandOutranksItsOffsuitTwin) {
    for (const HandClass hand : hand_classes_by_strength()) {
        if (hand.is_pair() || !hand.suited) continue;
        EXPECT_LT(strength_rank(hand), strength_rank(HandClass::of(hand.high, hand.low, false))) << hand.to_string();
    }
}

TEST(TopPercent, MatchesRequestedShareOfCombos) {
    for (const double percent : {12.0, 25.0, 55.0, 80.0}) {
        EXPECT_NEAR(HandRange::top_percent(percent).percent(), percent, 1.0) << percent;
    }
    EXPECT_TRUE(HandRange::top_percent(0.0).empty());
    EXPECT_EQ(HandRange::top_percent(100.0).combos(), kTotalCombos);
    EXPECT_TRUE(HandRange::top_percent(12.0).contains(hc("AA")));
    EXPECT_FALSE(HandRange::top_percent(12.0).contains(hc("72o")));
    EXPECT_TRUE(HandRange::top_percent(55.0).contains_all(HandRange::top_percent(12.0)));
}

// --- Range notation ----------------------------------------------------------

TEST(HandRangeParse, CountsCombosForEachForm) {
    EXPECT_EQ(HandRange::parse("AA").combos(), 6);
    EXPECT_EQ(HandRange::parse("22+").combos(), 78);
    EXPECT_EQ(HandRange::parse("JJ+").combos(), 24);
    EXPECT_EQ(HandRange::parse("JJ-88").combos(), 24);
    EXPECT_EQ(HandRange::parse("88-JJ").combos(), 24);
    EXPECT_EQ(HandRange::parse("A2s+").combos(), 48);
    EXPECT_EQ(HandRange::parse("ATo+").combos(), 48);
    EXPECT_EQ(HandRange::parse("AQs-ATs").combos(), 12);
    EXPECT_EQ(HandRange::parse("97s+").combos(), 8);
    EXPECT_EQ(HandRange::parse("AA, AKs, AKo").combos(), 22);
    EXPECT_TRUE(HandRange::parse("").empty());
    EXPECT_TRUE(HandRange::parse("   ").empty());
}

TEST(HandRangeParse, PlusStopsBelowThePair) {
    const HandRange range = HandRange::parse("97s+");
    EXPECT_TRUE(range.contains(hc("98s")));
    EXPECT_TRUE(range.contains(hc("97s")));
    EXPECT_FALSE(range.contains(hc("96s")));
    EXPECT_FALSE(range.contains(hc("99")));
}

TEST(HandRangeParse, RejectsBadNotation) {
    for (const char* text : {"XX", "AK", "AKs,", ",AKs", "AKs,,AQs", "AKs-QJs", "AKs-AQo", "AA-AKs", "AKs+x"}) {
        EXPECT_THROW(HandRange::parse(text), std::invalid_argument) << text;
    }
}

// --- RFI charts --------------------------------------------------------------

TEST(RfiCharts, WidthsAreInTheIntendedBands) {
    const auto expect_between = [](Position position, double low, double high) {
        const double percent = rfi_range(position).percent();
        EXPECT_GE(percent, low) << to_string(position);
        EXPECT_LE(percent, high) << to_string(position);
    };
    expect_between(Position::UTG, 15.0, 18.0);
    expect_between(Position::HJ, 19.0, 22.0);
    expect_between(Position::CO, 26.0, 30.0);
    expect_between(Position::BTN, 42.0, 50.0);
    expect_between(Position::SB, 40.0, 50.0);
    EXPECT_TRUE(rfi_range(Position::BB).empty());
}

TEST(RfiCharts, TightenByEarlierPosition) {
    EXPECT_TRUE(rfi_range(Position::HJ).contains_all(rfi_range(Position::UTG)));
    EXPECT_TRUE(rfi_range(Position::CO).contains_all(rfi_range(Position::HJ)));
    EXPECT_TRUE(rfi_range(Position::BTN).contains_all(rfi_range(Position::CO)));
    EXPECT_TRUE(rfi_range(Position::SB).contains_all(rfi_range(Position::CO)));
}

TEST(RfiCharts, KnownDecisions) {
    EXPECT_TRUE(should_open(hc("AA"), Position::UTG));
    EXPECT_TRUE(should_open(hc("A5s"), Position::UTG));
    EXPECT_FALSE(should_open(hc("KTo"), Position::UTG));
    EXPECT_TRUE(should_open(hc("KTo"), Position::HJ));
    EXPECT_FALSE(should_open(hc("22"), Position::HJ));
    EXPECT_TRUE(should_open(hc("22"), Position::CO));
    EXPECT_FALSE(should_open(hc("K2s"), Position::CO));
    EXPECT_TRUE(should_open(hc("K2s"), Position::BTN));
    EXPECT_FALSE(should_open(hc("72o"), Position::BTN));
    EXPECT_FALSE(should_open(hc("72o"), Position::SB));
    EXPECT_FALSE(should_open(hc("AA"), Position::BB));
}

TEST(RfiCharts, ShortHandedTablesStillResolve) {
    // Five players have no UTG and four no HJ; those seats use the same charts.
    for (const int players : {3, 4, 5}) {
        EXPECT_TRUE(rfi_range(Position::CO, players).contains_all(rfi_range(Position::HJ, players)));
        EXPECT_EQ(rfi_range(Position::BTN, players).combos(), rfi_range(Position::BTN).combos());
    }
    // Heads-up the button opens far wider, in position with the small blind posted.
    const HandRange& heads_up = rfi_range(Position::BTN, 2);
    EXPECT_GE(heads_up.percent(), 70.0);
    EXPECT_LE(heads_up.percent(), 85.0);
    EXPECT_TRUE(heads_up.contains_all(rfi_range(Position::BTN)));
    EXPECT_TRUE(should_open(hc("Q2o"), Position::BTN, 2));
    EXPECT_FALSE(should_open(hc("72o"), Position::BTN, 2));
}

// --- Responses to a raise ----------------------------------------------------

TEST(VsOpen, BigBlindDefendsWiderAgainstLaterOpeners) {
    const auto bb = [](Position opener) { return defended(vs_open_ranges(Position::BB, opener)); };
    EXPECT_TRUE(bb(Position::CO).contains_all(bb(Position::UTG)));
    EXPECT_TRUE(bb(Position::BTN).contains_all(bb(Position::CO)));
    EXPECT_TRUE(bb(Position::SB).contains_all(bb(Position::BTN)));
    EXPECT_GT(bb(Position::BTN).percent(), bb(Position::UTG).percent() + 10.0);
    EXPECT_TRUE(defended(vs_open_ranges(Position::BB, Position::BTN, 2)).contains_all(bb(Position::SB)));
}

TEST(VsOpen, EveryPositionPairDefendsThePremiumsAndFoldsTheWorst) {
    for (const Position opener : kOpeners) {
        for (const Position hero : {Position::HJ, Position::CO, Position::BTN, Position::SB, Position::BB}) {
            SCOPED_TRACE(std::string(to_string(hero)) + " vs " + std::string(to_string(opener)));
            EXPECT_EQ(vs_open_response(hc("AA"), hero, opener), PreflopResponse::Raise);
            EXPECT_EQ(vs_open_response(hc("KK"), hero, opener), PreflopResponse::Raise);
            EXPECT_EQ(vs_open_response(hc("AKs"), hero, opener), PreflopResponse::Raise);
            EXPECT_EQ(vs_open_response(hc("72o"), hero, opener), PreflopResponse::Fold);
        }
    }
}

TEST(VsOpen, ThreeBetRangesTightenAgainstEarlierOpeners) {
    const auto raises = [](Position hero, Position opener) { return vs_open_ranges(hero, opener).raise; };
    EXPECT_TRUE(raises(Position::BB, Position::CO).contains_all(raises(Position::BB, Position::UTG)));
    EXPECT_TRUE(raises(Position::SB, Position::CO).contains_all(raises(Position::SB, Position::UTG)));
    EXPECT_TRUE(raises(Position::SB, Position::BTN).contains_all(raises(Position::SB, Position::CO)));
    EXPECT_TRUE(raises(Position::BTN, Position::CO).contains_all(raises(Position::BTN, Position::UTG)));
}

TEST(VsOpen, KnownDecisions) {
    EXPECT_EQ(vs_open_response(hc("TT"), Position::BTN, Position::UTG), PreflopResponse::Call);
    EXPECT_EQ(vs_open_response(hc("A5s"), Position::BTN, Position::UTG), PreflopResponse::Raise);
    EXPECT_EQ(vs_open_response(hc("KTo"), Position::BTN, Position::UTG), PreflopResponse::Fold);
    EXPECT_EQ(vs_open_response(hc("K2s"), Position::BB, Position::BTN), PreflopResponse::Call);
    EXPECT_EQ(vs_open_response(hc("K2s"), Position::BB, Position::UTG), PreflopResponse::Fold);
    EXPECT_EQ(vs_open_response(hc("TT"), Position::SB, Position::BTN), PreflopResponse::Raise);
    EXPECT_EQ(vs_open_response(hc("76s"), Position::SB, Position::UTG), PreflopResponse::Fold);
}

TEST(VsReraises, RangesNarrowAsRaisesEscalate) {
    EXPECT_EQ(vs_three_bet_response(hc("AA")), PreflopResponse::Raise);
    EXPECT_EQ(vs_three_bet_response(hc("TT")), PreflopResponse::Call);
    EXPECT_EQ(vs_three_bet_response(hc("72o")), PreflopResponse::Fold);

    EXPECT_EQ(vs_four_bet_response(hc("AA")), PreflopResponse::Raise);
    EXPECT_EQ(vs_four_bet_response(hc("QQ")), PreflopResponse::Call);
    EXPECT_EQ(vs_four_bet_response(hc("TT")), PreflopResponse::Fold);
    EXPECT_EQ(vs_four_bet_response(hc("A5s")), PreflopResponse::Fold);
}

// --- Tiers -------------------------------------------------------------------

TEST(PreflopTier, NamesKnownHands) {
    EXPECT_EQ(preflop_tier(hc("AA")), PreflopTier::Premium);
    EXPECT_EQ(preflop_tier(hc("AKo")), PreflopTier::Premium);
    EXPECT_EQ(preflop_tier(hc("99")), PreflopTier::Strong);
    EXPECT_EQ(preflop_tier(hc("AJo")), PreflopTier::Strong);
    EXPECT_EQ(preflop_tier(hc("A5s")), PreflopTier::Playable);
    EXPECT_EQ(preflop_tier(hc("65s")), PreflopTier::Speculative);
    EXPECT_EQ(preflop_tier(hc("72o")), PreflopTier::Trash);
    EXPECT_EQ(to_string(preflop_tier(hc("AA"))), "premium");
    EXPECT_EQ(to_string(PreflopTier::Trash), "trash");
}

TEST(PreflopTier, NeverImprovesAsStrengthFalls) {
    int previous = 0;
    for (const HandClass hand : hand_classes_by_strength()) {
        const int tier = static_cast<int>(preflop_tier(hand));
        EXPECT_GE(tier, previous) << hand.to_string();
        previous = tier;
    }
}
