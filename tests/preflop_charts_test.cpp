#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "strategy/preflop_charts.h"

using namespace poker;

namespace {

HandClass hc(const char* text) { return HandClass::parse(text).value(); }

constexpr std::array<Position, 5> kOpeners{Position::UTG, Position::HJ, Position::CO, Position::BTN, Position::SB};
constexpr std::array<Position, 6> kAllPositions{Position::UTG, Position::HJ, Position::CO,
                                                Position::BTN, Position::SB, Position::BB};

PreflopResponse respond_for_test(const VsRaiseRanges& ranges, HandClass hand) {
    if (ranges.raise.contains(hand)) return PreflopResponse::Raise;
    return ranges.call.contains(hand) ? PreflopResponse::Call : PreflopResponse::Fold;
}

// Share of all 1326 combos, in percent.
double share(const HandRange& range) { return range.percent(); }

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

// --- The advisor's answers to an open ------------------------------------------

TEST(AdvisorVsOpen, SharesAreInTheirBands) {
    struct Band {
        Position hero;
        Position opener;
        int players;
        double raise_lo, raise_hi, defend_lo, defend_hi;
    };
    // 6-max 100 BB solver-style outputs: the BB defends ~55-65% against a BTN open
    // with ~12-15% 3-bets, 3-bets ~10-13% against the CO and ~8-10% against UTG/HJ;
    // the SB plays 3-bet or fold, ~10-15% against the BTN.
    const std::array<Band, 16> bands{{
        {Position::BB, Position::UTG, 6, 8.0, 10.0, 28.0, 36.0},
        {Position::BB, Position::HJ, 6, 8.0, 10.0, 34.0, 42.0},
        {Position::BB, Position::CO, 6, 10.0, 13.0, 44.0, 54.0},
        {Position::BB, Position::BTN, 6, 12.0, 15.0, 55.0, 65.0},
        {Position::BB, Position::SB, 6, 14.0, 19.0, 60.0, 72.0},
        {Position::BB, Position::BTN, 2, 14.0, 20.0, 65.0, 80.0},
        {Position::SB, Position::UTG, 6, 5.0, 8.0, 5.0, 8.0},
        {Position::SB, Position::HJ, 6, 6.5, 9.5, 6.5, 9.5},
        {Position::SB, Position::CO, 6, 8.5, 12.5, 8.5, 12.5},
        {Position::SB, Position::BTN, 6, 10.0, 15.0, 10.0, 15.0},
        {Position::HJ, Position::UTG, 6, 4.0, 6.5, 6.0, 11.0},
        {Position::CO, Position::UTG, 6, 4.0, 6.5, 7.0, 12.0},
        {Position::BTN, Position::UTG, 6, 4.0, 6.5, 9.0, 15.0},
        {Position::CO, Position::HJ, 6, 5.0, 8.0, 8.0, 13.0},
        {Position::BTN, Position::HJ, 6, 6.0, 9.0, 13.0, 19.0},
        {Position::BTN, Position::CO, 6, 8.0, 12.0, 15.0, 22.0},
    }};
    for (const Band& b : bands) {
        SCOPED_TRACE(std::string(to_string(b.hero)) + " vs " + std::string(to_string(b.opener)) + " " +
                     std::to_string(b.players));
        const VsRaiseRanges& r = advisor_vs_open_ranges(b.hero, b.opener, b.players);
        EXPECT_GE(share(r.raise), b.raise_lo);
        EXPECT_LE(share(r.raise), b.raise_hi);
        EXPECT_GE(share(defended(r)), b.defend_lo);
        EXPECT_LE(share(defended(r)), b.defend_hi);
        for (const HandClass hand : hand_classes_by_strength()) {
            EXPECT_FALSE(r.raise.contains(hand) && r.call.contains(hand)) << hand.to_string();
        }
    }
    // The SB plays 3-bet or fold.
    for (const Position opener : kOpeners) EXPECT_TRUE(advisor_vs_open_ranges(Position::SB, opener).call.empty());
}

TEST(AdvisorVsOpen, NestsAndIsWiderThanTheBots) {
    const auto bb = [](Position opener) { return advisor_vs_open_ranges(Position::BB, opener); };
    const std::array<Position, 5> later{Position::UTG, Position::HJ, Position::CO, Position::BTN, Position::SB};
    for (std::size_t i = 1; i < later.size(); ++i) {
        SCOPED_TRACE(std::string(to_string(later[i])));
        EXPECT_TRUE(bb(later[i]).raise.contains_all(bb(later[i - 1]).raise));
        EXPECT_TRUE(defended(bb(later[i])).contains_all(defended(bb(later[i - 1]))));
    }
    const VsRaiseRanges& heads_up = advisor_vs_open_ranges(Position::BB, Position::BTN, 2);
    EXPECT_TRUE(heads_up.raise.contains_all(bb(Position::SB).raise));
    EXPECT_TRUE(defended(heads_up).contains_all(defended(bb(Position::SB))));
    const auto sb = [](Position opener) { return advisor_vs_open_ranges(Position::SB, opener).raise; };
    EXPECT_TRUE(sb(Position::HJ).contains_all(sb(Position::UTG)));
    EXPECT_TRUE(sb(Position::CO).contains_all(sb(Position::HJ)));
    EXPECT_TRUE(sb(Position::BTN).contains_all(sb(Position::CO)));
    const auto ip = [](Position hero, Position opener) { return advisor_vs_open_ranges(hero, opener); };
    // Later heroes and later openers: the 3-bets and the defence widen.
    EXPECT_TRUE(ip(Position::CO, Position::UTG).raise.contains_all(ip(Position::HJ, Position::UTG).raise));
    EXPECT_TRUE(ip(Position::BTN, Position::UTG).raise.contains_all(ip(Position::CO, Position::UTG).raise));
    EXPECT_TRUE(ip(Position::BTN, Position::HJ).raise.contains_all(ip(Position::BTN, Position::UTG).raise));
    EXPECT_TRUE(ip(Position::BTN, Position::CO).raise.contains_all(ip(Position::BTN, Position::HJ).raise));
    EXPECT_TRUE(defended(ip(Position::CO, Position::UTG)).contains_all(defended(ip(Position::HJ, Position::UTG))));
    EXPECT_TRUE(defended(ip(Position::BTN, Position::UTG)).contains_all(defended(ip(Position::CO, Position::UTG))));
    EXPECT_TRUE(defended(ip(Position::BTN, Position::HJ)).contains_all(defended(ip(Position::BTN, Position::UTG))));
    EXPECT_TRUE(defended(ip(Position::BTN, Position::CO)).contains_all(defended(ip(Position::BTN, Position::HJ))));
    // The blinds defend and 3-bet more than in the bots' tables.
    for (const Position opener : kOpeners) {
        SCOPED_TRACE(std::string(to_string(opener)));
        EXPECT_GT(share(defended(bb(opener))), share(defended(vs_open_ranges(Position::BB, opener))));
        EXPECT_GT(share(bb(opener).raise), share(vs_open_ranges(Position::BB, opener).raise));
        EXPECT_GE(share(sb(opener)), share(vs_open_ranges(Position::SB, opener).raise));
    }
}

TEST(AdvisorVsOpen, ThreeBetsArePolarizedAndPremiumsAlwaysRaise) {
    for (const int players : {2, 6}) {
        for (const Position opener : kAllPositions) {
            for (const Position hero : kAllPositions) {
                if (players == 2 && hero != Position::BB) continue;
                SCOPED_TRACE(std::string(to_string(hero)) + " vs " + std::string(to_string(opener)));
                const VsRaiseRanges& r = advisor_vs_open_ranges(hero, opener, players);
                for (const char* premium : {"AA", "KK", "QQ", "AKs", "AKo"}) {
                    EXPECT_TRUE(r.raise.contains(hc(premium))) << premium;
                }
                EXPECT_FALSE(r.raise.contains(hc("72o")) || r.call.contains(hc("72o")));
                // A polarized part: a suited wheel ace, and outside the blinds' and
                // early cold 3-bets a suited connector too.
                EXPECT_TRUE(r.raise.contains(hc("A5s")) || r.raise.contains(hc("A4s")));
            }
        }
    }
    for (const Position opener : kOpeners) {
        const VsRaiseRanges& bb = advisor_vs_open_ranges(Position::BB, opener);
        EXPECT_TRUE(bb.raise.contains(hc("54s")) && bb.raise.contains(hc("76s"))) << to_string(opener);
    }
    EXPECT_TRUE(advisor_vs_open_ranges(Position::BTN, Position::CO).raise.contains(hc("65s")));
    EXPECT_TRUE(advisor_vs_open_ranges(Position::SB, Position::BTN).raise.contains(hc("87s")));
    // Known decisions.
    EXPECT_EQ(respond_for_test(advisor_vs_open_ranges(Position::BB, Position::BTN), hc("K2o")), PreflopResponse::Call);
    EXPECT_EQ(respond_for_test(advisor_vs_open_ranges(Position::BB, Position::CO), hc("TT")), PreflopResponse::Raise);
    EXPECT_EQ(respond_for_test(advisor_vs_open_ranges(Position::BB, Position::UTG), hc("K9o")), PreflopResponse::Fold);
    EXPECT_EQ(respond_for_test(advisor_vs_open_ranges(Position::BTN, Position::CO), hc("AJo")), PreflopResponse::Call);
    EXPECT_EQ(respond_for_test(advisor_vs_open_ranges(Position::SB, Position::CO), hc("AJo")), PreflopResponse::Raise);
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

// --- Positional responses to a 3-bet and a 4-bet (the advisor's) -----------------

namespace {

double continue_share(const VsRaiseRanges& ranges, const HandRange& open) {
    return 100.0 * defended(ranges).combos() / open.combos();
}

std::string spot_name(Position first, Position second, int players) {
    return std::string(to_string(first)) + " vs " + std::string(to_string(second)) + " (" +
           std::to_string(players) + " players)";
}

}  // namespace

TEST(PositionalVsThreeBet, NestsSensibly) {
    // Continue share of the opener's own open, by spot: the intended bands.
    struct Band {
        Position opener;
        Position three_bettor;
        int players;
        double low;
        double high;
    };
    const std::array<Band, 15> bands{{
        {Position::UTG, Position::HJ, 6, 38.0, 45.0},  {Position::UTG, Position::BTN, 6, 38.0, 45.0},
        {Position::UTG, Position::SB, 6, 46.0, 55.0},  {Position::UTG, Position::BB, 6, 46.0, 55.0},
        {Position::HJ, Position::CO, 6, 36.0, 44.0},   {Position::HJ, Position::BTN, 6, 36.0, 44.0},
        {Position::HJ, Position::BB, 6, 44.0, 53.0},   {Position::CO, Position::BTN, 6, 30.0, 40.0},
        {Position::CO, Position::SB, 6, 40.0, 50.0},   {Position::CO, Position::BB, 6, 40.0, 50.0},
        {Position::BTN, Position::SB, 6, 40.0, 55.0},  {Position::BTN, Position::BB, 6, 40.0, 55.0},
        {Position::SB, Position::BB, 6, 30.0, 40.0},   {Position::BTN, Position::BB, 2, 45.0, 60.0},
        {Position::CO, Position::BB, 4, 40.0, 50.0},
    }};
    for (const Band& band : bands) {
        SCOPED_TRACE(spot_name(band.opener, band.three_bettor, band.players));
        const double share = continue_share(vs_three_bet_ranges(band.opener, band.three_bettor, band.players),
                                            rfi_range(band.opener, band.players));
        EXPECT_GE(share, band.low);
        EXPECT_LE(share, band.high);
    }

    // Every continue hand is one the opener opens, and no hand is in both sets.
    for (std::size_t o = 0; o + 1 < kAllPositions.size(); ++o) {
        for (std::size_t t = o + 1; t < kAllPositions.size(); ++t) {
            const Position opener = kAllPositions[o];
            const Position three_bettor = kAllPositions[t];
            SCOPED_TRACE(spot_name(opener, three_bettor, kMaxSeats));
            const VsRaiseRanges& ranges = vs_three_bet_ranges(opener, three_bettor);
            EXPECT_TRUE(rfi_range(opener).contains_all(defended(ranges)));
            EXPECT_EQ(defended(ranges).combos(), ranges.raise.combos() + ranges.call.combos());
            EXPECT_TRUE(ranges.raise.contains(hc("AA")));
            EXPECT_TRUE(ranges.raise.contains(hc("KK")));
        }
    }
    EXPECT_TRUE(rfi_range(Position::BTN, 2).contains_all(defended(vs_three_bet_ranges(Position::BTN, Position::BB, 2))));

    // In position (against a blind 3-bet) an opener flats and continues a superset
    // of what it keeps out of position against a later 3-bettor.
    for (const Position opener : {Position::UTG, Position::HJ, Position::CO}) {
        SCOPED_TRACE(std::string(to_string(opener)));
        const VsRaiseRanges& ip = vs_three_bet_ranges(opener, Position::BB);
        const VsRaiseRanges& oop = vs_three_bet_ranges(opener, Position::BTN);
        EXPECT_TRUE(ip.call.contains_all(oop.call));
        EXPECT_TRUE(defended(ip).contains_all(defended(oop)));
        EXPECT_GT(defended(ip).combos(), defended(oop).combos());
        // Which blind 3-bets, or which later position, does not matter.
        EXPECT_EQ(&vs_three_bet_ranges(opener, Position::SB), &ip);
    }
    // Heads-up the button keeps more than it does six-handed.
    EXPECT_TRUE(defended(vs_three_bet_ranges(Position::BTN, Position::BB, 2))
                    .contains_all(defended(vs_three_bet_ranges(Position::BTN, Position::BB))));
}

TEST(PositionalVsThreeBet, EarlierOpenersKeepMoreOfTheirOpens) {
    const auto share = [](Position opener, Position three_bettor) {
        return continue_share(vs_three_bet_ranges(opener, three_bettor), rfi_range(opener));
    };
    EXPECT_GT(share(Position::UTG, Position::BTN), share(Position::HJ, Position::BTN));
    EXPECT_GT(share(Position::HJ, Position::BTN), share(Position::CO, Position::BTN));
    EXPECT_GT(share(Position::UTG, Position::BB), share(Position::HJ, Position::BB));
    EXPECT_GT(share(Position::HJ, Position::BB), share(Position::CO, Position::BB));
    // In absolute terms the later, wider opens still continue more hands.
    EXPECT_GT(defended(vs_three_bet_ranges(Position::CO, Position::BB)).combos(),
              defended(vs_three_bet_ranges(Position::UTG, Position::BB)).combos());
}

TEST(PositionalVsThreeBet, KnownDecisions) {
    const auto at = [](const char* hand, Position opener, Position three_bettor, int players = kMaxSeats) {
        return respond_for_test(vs_three_bet_ranges(opener, three_bettor, players), hc(hand));
    };
    // The cutoff defends a BB 3-bet with its good broadways and suited hands.
    EXPECT_EQ(at("AA", Position::CO, Position::BB), PreflopResponse::Raise);
    EXPECT_EQ(at("A5s", Position::CO, Position::BB), PreflopResponse::Raise);
    EXPECT_EQ(at("77", Position::CO, Position::BB), PreflopResponse::Call);
    EXPECT_EQ(at("AJo", Position::CO, Position::BB), PreflopResponse::Call);
    EXPECT_EQ(at("76s", Position::CO, Position::BB), PreflopResponse::Call);
    EXPECT_EQ(at("AJo", Position::CO, Position::BTN), PreflopResponse::Fold);
    EXPECT_EQ(at("72o", Position::CO, Position::BB), PreflopResponse::Fold);
    EXPECT_EQ(at("K5s", Position::CO, Position::BB), PreflopResponse::Fold);
    // UTG keeps its strong hands either way.
    EXPECT_EQ(at("JJ", Position::UTG, Position::BTN), PreflopResponse::Call);
    EXPECT_EQ(at("AKo", Position::UTG, Position::BTN), PreflopResponse::Call);
    EXPECT_EQ(at("T9s", Position::UTG, Position::BTN), PreflopResponse::Fold);
    EXPECT_EQ(at("T9s", Position::UTG, Position::BB), PreflopResponse::Call);
    // The button and the heads-up button play back widely.
    EXPECT_EQ(at("54s", Position::BTN, Position::BB), PreflopResponse::Call);
    EXPECT_EQ(at("K2s", Position::BTN, Position::BB), PreflopResponse::Fold);
    EXPECT_EQ(at("K2s", Position::BTN, Position::BB, 2), PreflopResponse::Call);
    EXPECT_EQ(at("A2o", Position::BTN, Position::BB, 2), PreflopResponse::Call);
    EXPECT_EQ(at("72o", Position::BTN, Position::BB, 2), PreflopResponse::Fold);
}

TEST(PositionalVsFourBet, VsFourBetRangesAreNarrow) {
    struct Spot {
        Position three_bettor;
        Position four_bettor;
        int players;
        double max_percent;
    };
    const std::array<Spot, 7> spots{{
        {Position::BTN, Position::UTG, 6, 5.0},
        {Position::HJ, Position::UTG, 6, 5.0},
        {Position::BTN, Position::CO, 6, 6.0},
        {Position::BB, Position::CO, 6, 5.0},
        {Position::SB, Position::BTN, 6, 5.0},
        {Position::BB, Position::SB, 6, 8.0},
        {Position::BB, Position::BTN, 2, 12.0},
    }};
    for (const Spot& spot : spots) {
        SCOPED_TRACE(spot_name(spot.three_bettor, spot.four_bettor, spot.players));
        const VsRaiseRanges& ranges = vs_four_bet_ranges(spot.three_bettor, spot.four_bettor, spot.players);
        const HandRange all = defended(ranges);
        EXPECT_GT(all.percent(), 2.0);
        EXPECT_LE(all.percent(), spot.max_percent);
        EXPECT_EQ(all.combos(), ranges.raise.combos() + ranges.call.combos());
        // The 5-bet jam is premiums only, and always includes aces and kings.
        for (const HandClass hand : hand_classes_by_strength()) {
            if (ranges.raise.contains(hand)) {
                EXPECT_EQ(preflop_tier(hand), PreflopTier::Premium) << hand.to_string();
            }
        }
        EXPECT_TRUE(ranges.raise.contains(hc("AA")));
        EXPECT_TRUE(ranges.raise.contains(hc("KK")));
        EXPECT_FALSE(all.contains(hc("A5s")));
        // Fewer hands answer the 4-bet than the 4-bettor kept against the 3-bet.
        EXPECT_LT(all.combos(),
                  defended(vs_three_bet_ranges(spot.four_bettor, spot.three_bettor, spot.players)).combos());
    }

    // Out of position a blind 3-bettor jams more and flats less than one in position.
    const VsRaiseRanges& ip = vs_four_bet_ranges(Position::BTN, Position::CO);
    const VsRaiseRanges& oop = vs_four_bet_ranges(Position::BB, Position::CO);
    EXPECT_GT(oop.raise.combos(), ip.raise.combos());
    EXPECT_LT(oop.call.combos(), ip.call.combos());
    // Wider 4-bets are answered wider: early < late < blind vs blind < heads-up.
    const auto continued = [](Position three_bettor, Position four_bettor, int players = kMaxSeats) {
        return defended(vs_four_bet_ranges(three_bettor, four_bettor, players));
    };
    EXPECT_TRUE(continued(Position::BTN, Position::CO).contains_all(continued(Position::BTN, Position::UTG)));
    EXPECT_TRUE(continued(Position::BTN, Position::CO).contains_all(continued(Position::BB, Position::CO)));
    EXPECT_TRUE(continued(Position::BB, Position::SB).contains_all(continued(Position::BB, Position::CO)));
    EXPECT_TRUE(continued(Position::BB, Position::BTN, 2).contains_all(continued(Position::BB, Position::SB)));
    EXPECT_TRUE(continued(Position::BB, Position::BTN, 2).contains_all(continued(Position::BTN, Position::CO)));
}

// The bots' non-positional charts, snapshotted before the positional tables were
// added: one letter per class in HandClass::index() order (the 13x13 grid, AA
// first, suited above the diagonal), F fold, C call, R raise.
TEST(VsReraises, BotsChartsUnchanged) {
    constexpr std::string_view kVsThreeBet =
        "RRCCCFFFFRFFF"
        "RRCCFFFFFFFFF"
        "CFRCFFFFFFFFF"
        "FFFCCFFFFFFFF"
        "FFFFCCFFFFFFF"
        "FFFFFCFFFFFFF"
        "FFFFFFCFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF";
    constexpr std::string_view kVsFourBet =
        "RRFFFFFFFFFFF"
        "CRFFFFFFFFFFF"
        "FFCFFFFFFFFFF"
        "FFFCFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF";
    static_assert(kVsThreeBet.size() == kHandClassCount && kVsFourBet.size() == kHandClassCount);
    const auto letter = [](PreflopResponse response) { return to_string(response)[0]; };
    for (const HandClass hand : hand_classes_by_strength()) {
        const auto index = static_cast<std::size_t>(hand.index());
        EXPECT_EQ(letter(vs_three_bet_response(hand)), kVsThreeBet[index]) << hand.to_string();
        EXPECT_EQ(letter(vs_four_bet_response(hand)), kVsFourBet[index]) << hand.to_string();
    }
    EXPECT_EQ(vs_three_bet_response(hc("TT")), PreflopResponse::Call);
    EXPECT_EQ(vs_three_bet_response(hc("A5s")), PreflopResponse::Raise);
    EXPECT_EQ(vs_four_bet_response(hc("JJ")), PreflopResponse::Call);

    // The bots' answers to an open, snapshotted when the advisor got its own
    // tables (Stage 6 follow-ups): the ten tables, then which one every hero,
    // opener and table size reads.
    constexpr std::array<std::string_view, 10> kVsOpen{
        // kInPositionVsEarly
        "RRCCCFFFFRRFF"
        "RRCCFFFFFFFFF"
        "CFRCFFFFFFFFF"
        "FFFCCFFFFFFFF"
        "FFFFCCFFFFFFF"
        "FFFFFCCFFFFFF"
        "FFFFFFCFFFFFF"
        "FFFFFFFCFFFFF"
        "FFFFFFFFCFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF",
        // kInPositionVsLate
        "RRRCCCCFFRRRF"
        "RRRRCFFFFFFFF"
        "RCRCCFFFFFFFF"
        "CFFRCCFFFFFFF"
        "FFFFCCFFFFFFF"
        "FFFFFCCFFFFFF"
        "FFFFFFCCFFFFF"
        "FFFFFFFCFFFFF"
        "FFFFFFFFCFFFF"
        "FFFFFFFFFCFFF"
        "FFFFFFFFFFCFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF",
        // kSbVsEarly
        "RRRFFFFFFRRFF"
        "RRRFFFFFFFFFF"
        "FFRFFFFFFFFFF"
        "FFFRFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF",
        // kSbVsCutoff
        "RRRRRFFFFRRRR"
        "RRRRRFFFFFFFF"
        "RFRRFFFFFFFFF"
        "FFFRFFFFFFFFF"
        "FFFFRFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF",
        // kSbVsButton
        "RRRRRRRFFRRRR"
        "RRRRRFFFFFFFF"
        "RRRRRFFFFFFFF"
        "RRFRRFFFFFFFF"
        "RFFFRRFFFFFFF"
        "FFFFFRFFFFFFF"
        "FFFFFFRFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF"
        "FFFFFFFFFFFFF",
        // kBbVsEarly
        "RRCCCCCCCRCCC"
        "RRCCCCFFFFFFF"
        "CCRCCCFFFFFFF"
        "CCFCCCFFFFFFF"
        "CFFFCCCFFFFFF"
        "FFFFFCCFFFFFF"
        "FFFFFFCCFFFFF"
        "FFFFFFFCCFFFF"
        "FFFFFFFFCCFFF"
        "FFFFFFFFFCCFF"
        "FFFFFFFFFFCFF"
        "FFFFFFFFFFFCF"
        "FFFFFFFFFFFFC",
        // kBbVsCutoff
        "RRRCCCCCCRRRC"
        "RRRCCCCCCFFFF"
        "CCRCCCCFFFFFF"
        "CCCRCCCFFFFFF"
        "CCCCCCCCFFFFF"
        "CFFFFCCCFFFFF"
        "CFFFFFCCCFFFF"
        "FFFFFFFCCCFFF"
        "FFFFFFFFCCCFF"
        "FFFFFFFFFCCFF"
        "FFFFFFFFFFCFF"
        "FFFFFFFFFFFCF"
        "FFFFFFFFFFFFC",
        // kBbVsButton
        "RRRRCCCCCRRRR"
        "RRRCCCCCCCCCC"
        "RCRCCCCCCCFFF"
        "CCCRCCCCCFFFF"
        "CCCCRCCCCFFFF"
        "CCCCCCCCCFFFF"
        "CCFFCCCCCCFFF"
        "CFFFFFCCCCCFF"
        "CFFFFFFFCCCCF"
        "CFFFFFFFFCCCF"
        "CFFFFFFFFFCCF"
        "FFFFFFFFFFFCF"
        "FFFFFFFFFFFFC",
        // kBbVsSb
        "RRRRRCCCCRRRR"
        "RRRRCCCCCCCCC"
        "RCRCCCCCCCCCC"
        "RCCRCCCCCCCFF"
        "CCCCRCCCCFFFF"
        "CCCCCRCCCCFFF"
        "CCCCCCCCCCFFF"
        "CCFFFCCCCCCFF"
        "CFFFFFFCCCCCF"
        "CFFFFFFFFCCCF"
        "CFFFFFFFFFCCF"
        "CFFFFFFFFFFCF"
        "CFFFFFFFFFFFC",
        // kBbHeadsUp
        "RRRRRCCCCRRRR"
        "RRRRCCCCCCCCC"
        "RCRCCCCCCCCCC"
        "RCCRCCCCCCCCC"
        "CCCCRCCCCCCCF"
        "CCCCCRCCCCCCF"
        "CCCCCCCCCCCFF"
        "CCCCCCCCCCCFF"
        "CCCFFFCCCCCCF"
        "CCCFFFFFCCCCC"
        "CCFFFFFFFFCCC"
        "CCFFFFFFFFFCC"
        "CCFFFFFFFFFFC"};
    enum Table : std::uint8_t { kIpEarly, kIpLate, kSbEarly, kSbCo, kSbBtn, kBbEarly, kBbCo, kBbBtn, kBbSb, kBbHu };
    const auto table_for = [](Position hero, Position opener, int players) {
        const bool early = opener == Position::UTG || opener == Position::HJ;
        if (hero == Position::BB) {
            if (players == 2) return kBbHu;
            if (early) return kBbEarly;
            return opener == Position::CO ? kBbCo : opener == Position::BTN ? kBbBtn : kBbSb;
        }
        if (hero == Position::SB) return early ? kSbEarly : opener == Position::CO ? kSbCo : kSbBtn;
        return early ? kIpEarly : kIpLate;
    };
    for (const int players : {2, 6}) {
        for (const Position hero : kAllPositions) {
            for (const Position opener : kAllPositions) {
                const std::string_view expected = kVsOpen[table_for(hero, opener, players)];
                for (const HandClass hand : hand_classes_by_strength()) {
                    const auto index = static_cast<std::size_t>(hand.index());
                    EXPECT_EQ(letter(vs_open_response(hand, hero, opener, players)), expected[index])
                        << to_string(hero) << " vs " << to_string(opener) << " " << players << " " << hand.to_string();
                }
            }
        }
    }
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
