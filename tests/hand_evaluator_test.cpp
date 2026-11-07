#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/deck.h"
#include "core/hand_evaluator.h"

using namespace poker;

namespace {

// Parses e.g. "As Ks Qs Js Ts 2d 3c".
std::array<Card, 7> hand(const std::string& text) {
    std::array<Card, 7> cards;
    std::istringstream in(text);
    std::string token;
    for (Card& card : cards) {
        in >> token;
        card = Card::parse(token).value();
    }
    return cards;
}

HandRank rank_of(const std::string& text) { return evaluate7(hand(text)); }

}  // namespace

TEST(HandEvaluator, RoyalFlushIsRankOne) {
    EXPECT_EQ(rank_of("As Ks Qs Js Ts 2d 3c"), kBestHandRank);
    EXPECT_EQ(describe_category(kBestHandRank), "Straight Flush");
}

TEST(HandEvaluator, WheelStraightFlushIsLowestStraightFlush) {
    EXPECT_EQ(rank_of("5h 4h 3h 2h Ah Kd Qc"), 10);
}

TEST(HandEvaluator, CategoryIdentification) {
    const std::map<std::string, HandCategory> cases = {
        {"9h 8h 7h 6h 5h Ad Kc", HandCategory::StraightFlush},
        {"Ac Ad Ah As 2c 3d 7h", HandCategory::FourOfAKind},
        {"Ac Ad Ah Ks Kd 3d 7h", HandCategory::FullHouse},
        {"Ah Kh 9h 5h 2h Qd Jc", HandCategory::Flush},
        {"9c 8d 7h 6s 5c Kd 2h", HandCategory::Straight},
        {"5c 4d 3h 2s Ac Kd 9h", HandCategory::Straight},  // Wheel
        {"Ac Ad Ah Ks Qd 3d 7h", HandCategory::ThreeOfAKind},
        {"Ac Ad Ks Kd Qd 3d 7h", HandCategory::TwoPair},
        {"Ac Ad Ks Qd Jh 3d 7h", HandCategory::OnePair},
        {"Ac Kd Qs Jd 9h 3d 7h", HandCategory::HighCard},
    };
    for (const auto& [text, category] : cases) {
        EXPECT_EQ(hand_category(rank_of(text)), category) << text;
    }
}

TEST(HandEvaluator, CategoryRankBoundaries) {
    const std::array<std::pair<HandRank, HandCategory>, 18> boundaries{{
        {1, HandCategory::StraightFlush},  {10, HandCategory::StraightFlush},
        {11, HandCategory::FourOfAKind},   {166, HandCategory::FourOfAKind},
        {167, HandCategory::FullHouse},    {322, HandCategory::FullHouse},
        {323, HandCategory::Flush},        {1599, HandCategory::Flush},
        {1600, HandCategory::Straight},    {1609, HandCategory::Straight},
        {1610, HandCategory::ThreeOfAKind},{2467, HandCategory::ThreeOfAKind},
        {2468, HandCategory::TwoPair},     {3325, HandCategory::TwoPair},
        {3326, HandCategory::OnePair},     {6185, HandCategory::OnePair},
        {6186, HandCategory::HighCard},    {kWorstHandRank, HandCategory::HighCard},
    }};
    for (const auto& [rank, category] : boundaries) {
        EXPECT_EQ(hand_category(rank), category) << rank;
    }
}

TEST(HandEvaluator, CategoryOrderingHoldsAcrossHands) {
    const std::vector<std::string> strongest_to_weakest = {
        "As Ks Qs Js Ts 2d 3c",  // Straight flush
        "Ac Ad Ah As 2c 3d 7h",  // Quads
        "Ac Ad Ah Ks Kd 3d 7h",  // Full house
        "Ah Kh 9h 5h 2h Qd Jc",  // Flush
        "9c 8d 7h 6s 5c Kd 2h",  // Straight
        "Ac Ad Ah Ks Qd 3d 7h",  // Trips
        "Ac Ad Ks Kd Qd 3d 7h",  // Two pair
        "Ac Ad Ks Qd Jh 3d 7h",  // One pair
        "Ac Kd Qs Jd 9h 3d 7h",  // High card
    };
    for (std::size_t i = 1; i < strongest_to_weakest.size(); ++i) {
        EXPECT_LT(rank_of(strongest_to_weakest[i - 1]), rank_of(strongest_to_weakest[i]))
            << strongest_to_weakest[i - 1] << " vs " << strongest_to_weakest[i];
    }
}

TEST(HandEvaluator, KickerBreaksTie) {
    EXPECT_LT(rank_of("Ac Kd Ah 7s 5d 3c 2h"), rank_of("Ac Qd Ah 7s 5d 3c 2h"));
}

TEST(HandEvaluator, CardsOutsideBestFiveAreIgnored) {
    // Best five is A-A-K-Q-J either way; the 3 vs 2 can't play.
    EXPECT_EQ(rank_of("Ac Ad Kh Qs Jd 3c 2h"), rank_of("Ah As Kd Qc Jh 4c 2d"));
}

TEST(HandEvaluator, BoardPlaysGivesTie) {
    const std::array<Card, 5> board{*Card::parse("As"), *Card::parse("Ks"), *Card::parse("Qd"),
                                    *Card::parse("Jc"), *Card::parse("Th")};
    const std::array<Card, 2> first{*Card::parse("2c"), *Card::parse("3d")};
    const std::array<Card, 2> second{*Card::parse("4h"), *Card::parse("5c")};
    EXPECT_EQ(evaluate7(first, board), evaluate7(second, board));
    EXPECT_EQ(hand_category(evaluate7(first, board)), HandCategory::Straight);
}

TEST(HandEvaluator, HoleAndBoardOverloadMatchesFlatArray) {
    const std::array<Card, 2> hole{*Card::parse("Ah"), *Card::parse("Ad")};
    const std::array<Card, 5> board{*Card::parse("Ac"), *Card::parse("Ks"), *Card::parse("Kd"),
                                    *Card::parse("3d"), *Card::parse("7h")};
    EXPECT_EQ(evaluate7(hole, board), rank_of("Ah Ad Ac Ks Kd 3d 7h"));
}

TEST(HandEvaluator, BestFiveAreSelectedFromSeven) {
    // Two sets of trips make a full house, using the higher as the trips.
    EXPECT_EQ(hand_category(rank_of("Ac Ad Ah Ks Kd Kh 2c")), HandCategory::FullHouse);
    EXPECT_EQ(rank_of("Ac Ad Ah Ks Kd Kh 2c"), rank_of("Ac Ad Ah Ks Kd 3c 2c"));

    // Three pairs: two best pairs plus the best remaining kicker.
    EXPECT_EQ(rank_of("Ac Ad Ks Kd Qs Qd 2c"), rank_of("Ac Ad Ks Kd Qs 3d 2c"));

    // Six to a flush: only the five highest count.
    EXPECT_EQ(rank_of("Ah Kh Qh Jh 9h 3h 2d"), rank_of("Ah Kh Qh Jh 9h 2h 2d"));

    // Flush beats a straight when both are available.
    EXPECT_EQ(hand_category(rank_of("9h 8h 7h 6h 2h Td 5c")), HandCategory::Flush);

    // Six-card straight plays the top end.
    EXPECT_EQ(rank_of("Tc 9d 8h 7s 6c 5d 2h"), rank_of("Tc 9d 8h 7s 6c Kd 2h"));
}

TEST(HandEvaluator, ResultIndependentOfCardOrder) {
    auto cards = hand("Ac Kd Qs 9h 7d 3c 2h");
    const HandRank expected = evaluate7(cards);
    std::sort(cards.begin(), cards.end());
    do {
        ASSERT_EQ(evaluate7(cards), expected);
    } while (std::next_permutation(cards.begin(), cards.end()));
}

TEST(HandEvaluator, ResultIndependentOfSuitRelabelling) {
    const HandRank expected = rank_of("Ah Kh 9h 5h 2h Qd Jc");
    EXPECT_EQ(rank_of("As Ks 9s 5s 2s Qc Jd"), expected);
    EXPECT_EQ(rank_of("Ac Kc 9c 5c 2c Qh Js"), expected);
}

TEST(HandEvaluator, NamesTheCurrentHandInEveryCategory) {
    EXPECT_EQ(describe_made_hand(rank_of("As Ks Qs Js Ts 2d 3c")), "Royal Flush");
    EXPECT_EQ(describe_made_hand(rank_of("9s 8s 7s 6s 5s 2d 3c")), "Nine-High Straight Flush");
    EXPECT_EQ(describe_made_hand(rank_of("Ac Ad Ah As Kd 3d 7h")), "Four Aces");
    EXPECT_EQ(describe_made_hand(rank_of("Ac Ad Ah Ks Kd 3d 7h")), "Aces Full over Kings");
    EXPECT_EQ(describe_made_hand(rank_of("As Ks 9s 7s 2s 3d 4h")), "Ace-High Flush");
    EXPECT_EQ(describe_made_hand(rank_of("Ts 9d 8c 7h 6s 2d 2c")), "Ten-High Straight");
    EXPECT_EQ(describe_made_hand(rank_of("Ac Ad Ah Ks Qd 3d 7h")), "Three Aces");
    EXPECT_EQ(describe_made_hand(rank_of("Ac Ad Ks Kd 9h 3d 7h")), "Two Pair, Aces and Kings");
    EXPECT_EQ(describe_made_hand(rank_of("Kc Kd As 9d 7h 3d 2h")), "Pair of Kings");
    EXPECT_EQ(describe_made_hand(rank_of("Ac Qd 9s 7d 5h 3d 2h")), "Ace High");
}

TEST(HandEvaluator, NamesTheCurrentHandFromFiveAndSixCards) {
    const std::array<Card, 7> c = {Card(Rank::Ace, Suit::Clubs),  Card(Rank::Ace, Suit::Diamonds),
                                   Card(Rank::King, Suit::Spades), Card(Rank::King, Suit::Diamonds),
                                   Card(Rank::Nine, Suit::Hearts), Card(Rank::Three, Suit::Diamonds),
                                   Card(Rank::Seven, Suit::Hearts)};
    EXPECT_EQ(describe_made_hand(evaluate(std::span<const Card>(c.data(), 5))), "Two Pair, Aces and Kings");
    EXPECT_EQ(describe_made_hand(evaluate(std::span<const Card>(c.data(), 6))), "Two Pair, Aces and Kings");
    EXPECT_EQ(describe_made_hand(evaluate(std::span<const Card>(c.data(), 7))), "Two Pair, Aces and Kings");
}

TEST(HandEvaluator, DescribesHands) {
    EXPECT_EQ(describe_category(rank_of("Ac Ad Ah Ks Kd 3d 7h")), "Full House");
    EXPECT_FALSE(describe_hand(rank_of("Ac Ad Ah Ks Kd 3d 7h")).empty());
}

TEST(HandEvaluator, FiveCardHandsOnFlop) {
    const auto cards = [](const char* a, const char* b, const char* c, const char* d, const char* e) {
        return std::array<Card, 5>{*Card::parse(a), *Card::parse(b), *Card::parse(c),
                                   *Card::parse(d), *Card::parse(e)};
    };
    EXPECT_EQ(evaluate5(cards("As", "Ks", "Qs", "Js", "Ts")), kBestHandRank);
    EXPECT_EQ(evaluate5(cards("7c", "5d", "4h", "3s", "2c")), kWorstHandRank);
    EXPECT_EQ(hand_category(evaluate5(cards("Ah", "Ad", "Kc", "Ks", "Kd"))), HandCategory::FullHouse);
}

TEST(HandEvaluator, SixCardHandsOnTurn) {
    const std::array<Card, 6> set_over_pair{*Card::parse("9h"), *Card::parse("9d"), *Card::parse("9c"),
                                            *Card::parse("Ks"), *Card::parse("Kd"), *Card::parse("2c")};
    EXPECT_EQ(hand_category(evaluate6(set_over_pair)), HandCategory::FullHouse);

    const std::array<Card, 6> royal{*Card::parse("As"), *Card::parse("Ks"), *Card::parse("Qs"),
                                    *Card::parse("Js"), *Card::parse("Ts"), *Card::parse("2c")};
    EXPECT_EQ(evaluate6(royal), kBestHandRank);
}

TEST(HandEvaluator, SpanDispatchMatchesFixedSizeFunctions) {
    const auto seven = hand("Ac Ad Ks Qd Jh 3d 7h");
    const std::span<const Card> all(seven);

    EXPECT_EQ(evaluate(all.first(5)), evaluate5({seven[0], seven[1], seven[2], seven[3], seven[4]}));
    EXPECT_EQ(evaluate(all.first(6)), evaluate6({seven[0], seven[1], seven[2], seven[3], seven[4], seven[5]}));
    EXPECT_EQ(evaluate(all), evaluate7(seven));
    EXPECT_THROW(evaluate(all.first(4)), std::invalid_argument);
}

// Every C(52,5) = 2,598,960 five-card hand against the published frequencies.
TEST(HandEvaluator, ExhaustiveFiveCardFrequencies) {
    std::vector<std::uint32_t> rank_counts(kWorstHandRank + 1, 0);
    for (int a = 0; a < 48; ++a) {
        for (int b = a + 1; b < 49; ++b) {
            for (int c = b + 1; c < 50; ++c) {
                for (int d = c + 1; d < 51; ++d) {
                    for (int e = d + 1; e < 52; ++e) {
                        ++rank_counts[evaluate5({Card(a), Card(b), Card(c), Card(d), Card(e)})];
                    }
                }
            }
        }
    }

    std::map<HandCategory, std::uint64_t> by_category;
    int distinct = 0;
    for (HandRank rank = kBestHandRank; rank <= kWorstHandRank; ++rank) {
        by_category[hand_category(rank)] += rank_counts[rank];
        if (rank_counts[rank] > 0) ++distinct;
    }

    EXPECT_EQ(rank_counts[0], 0u);
    EXPECT_EQ(distinct, 7462);
    EXPECT_EQ(by_category[HandCategory::StraightFlush], 40u);
    EXPECT_EQ(by_category[HandCategory::FourOfAKind], 624u);
    EXPECT_EQ(by_category[HandCategory::FullHouse], 3'744u);
    EXPECT_EQ(by_category[HandCategory::Flush], 5'108u);
    EXPECT_EQ(by_category[HandCategory::Straight], 10'200u);
    EXPECT_EQ(by_category[HandCategory::ThreeOfAKind], 54'912u);
    EXPECT_EQ(by_category[HandCategory::TwoPair], 123'552u);
    EXPECT_EQ(by_category[HandCategory::OnePair], 1'098'240u);
    EXPECT_EQ(by_category[HandCategory::HighCard], 1'302'540u);
}

// The 6-card evaluator uses its own hash tables, so checking it against the
// best 5-card subset cross-validates the two independently, over all
// C(52,6) = 20,358,520 hands.
TEST(HandEvaluator, ExhaustiveSixCardMatchesBestFiveCardSubset) {
    std::uint64_t mismatches = 0;
    for (int a = 0; a < 47; ++a) {
        for (int b = a + 1; b < 48; ++b) {
            for (int c = b + 1; c < 49; ++c) {
                for (int d = c + 1; d < 50; ++d) {
                    for (int e = d + 1; e < 51; ++e) {
                        for (int f = e + 1; f < 52; ++f) {
                            const std::array<Card, 6> six{Card(a), Card(b), Card(c), Card(d), Card(e), Card(f)};
                            HandRank best = kWorstHandRank + 1;
                            for (std::size_t skip = 0; skip < 6; ++skip) {
                                std::array<Card, 5> five;
                                std::size_t n = 0;
                                for (std::size_t i = 0; i < 6; ++i) {
                                    if (i != skip) five[n++] = six[i];
                                }
                                best = std::min(best, evaluate5(five));
                            }
                            if (evaluate6(six) != best) ++mismatches;
                        }
                    }
                }
            }
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

TEST(HandEvaluator, SevenCardMatchesBestSixCardSubsetOnRandomHands) {
    std::mt19937_64 rng(2024);
    for (int trial = 0; trial < 1'000'000; ++trial) {
        Deck deck;
        deck.shuffle(rng);
        std::array<Card, 7> seven;
        for (Card& card : seven) card = deck.deal();

        HandRank best = kWorstHandRank + 1;
        for (std::size_t skip = 0; skip < 7; ++skip) {
            std::array<Card, 6> six;
            std::size_t n = 0;
            for (std::size_t i = 0; i < 7; ++i) {
                if (i != skip) six[n++] = seven[i];
            }
            best = std::min(best, evaluate6(six));
        }
        ASSERT_EQ(evaluate7(seven), best) << "trial " << trial;
    }
}

// Evaluates every one of the C(52,7) = 133,784,560 seven-card hands and checks
// the totals against the published combinatorial frequencies. This is the
// strongest check here: a table or wiring error would shift these counts.
TEST(HandEvaluator, ExhaustiveSevenCardFrequencies) {
    std::vector<std::uint32_t> rank_counts(kWorstHandRank + 1, 0);
    std::array<Card, 7> c;
    for (int a = 0; a < 46; ++a) {
        c[0] = Card(a);
        for (int b = a + 1; b < 47; ++b) {
            c[1] = Card(b);
            for (int d = b + 1; d < 48; ++d) {
                c[2] = Card(d);
                for (int e = d + 1; e < 49; ++e) {
                    c[3] = Card(e);
                    for (int f = e + 1; f < 50; ++f) {
                        c[4] = Card(f);
                        for (int g = f + 1; g < 51; ++g) {
                            c[5] = Card(g);
                            for (int h = g + 1; h < 52; ++h) {
                                c[6] = Card(h);
                                ++rank_counts[evaluate7(c)];
                            }
                        }
                    }
                }
            }
        }
    }

    std::map<HandCategory, std::uint64_t> by_category;
    std::uint64_t total = 0;
    int distinct = 0;
    for (HandRank rank = kBestHandRank; rank <= kWorstHandRank; ++rank) {
        if (rank_counts[rank] == 0) continue;
        by_category[hand_category(rank)] += rank_counts[rank];
        total += rank_counts[rank];
        ++distinct;
    }

    EXPECT_EQ(rank_counts[0], 0u);
    EXPECT_EQ(total, 133'784'560u);
    EXPECT_EQ(distinct, 4824);
    EXPECT_EQ(rank_counts[kBestHandRank], 4'324u);  // Royal flushes
    EXPECT_EQ(by_category[HandCategory::StraightFlush], 41'584u);
    EXPECT_EQ(by_category[HandCategory::FourOfAKind], 224'848u);
    EXPECT_EQ(by_category[HandCategory::FullHouse], 3'473'184u);
    EXPECT_EQ(by_category[HandCategory::Flush], 4'047'644u);
    EXPECT_EQ(by_category[HandCategory::Straight], 6'180'020u);
    EXPECT_EQ(by_category[HandCategory::ThreeOfAKind], 6'461'620u);
    EXPECT_EQ(by_category[HandCategory::TwoPair], 31'433'400u);
    EXPECT_EQ(by_category[HandCategory::OnePair], 58'627'800u);
    EXPECT_EQ(by_category[HandCategory::HighCard], 23'294'460u);
}

// --- Best five cards and kickers ---------------------------------------------

namespace {

std::array<Card, 2> hole_of(const std::string& text) {
    const auto all = hand(text + " 2c 3c 4c 5c 6c");
    return {all[0], all[1]};
}

std::array<Card, 5> board_of(const std::string& text) {
    const auto all = hand("Ah Ad " + text);
    return {all[2], all[3], all[4], all[5], all[6]};
}

std::string ranks_of(const std::array<Card, 5>& five) {
    std::string out;
    for (const Card& c : five) out += c.to_string()[0];
    return out;
}

}  // namespace

TEST(BestFive, OrdersPairThenKickers) {
    EXPECT_EQ(ranks_of(best_five(hole_of("Kc Kd"), board_of("As 9d 7h 3d 2h"))), "KKA97");
}

TEST(BestFive, TwoPairKeepsTheBestKicker) {
    EXPECT_EQ(ranks_of(best_five(hole_of("Ac Kd"), board_of("As Kh 9d 9c 3h"))), "AAKK9");
    EXPECT_EQ(ranks_of(best_five(hole_of("Qc Jd"), board_of("As Ah Kd Kc 3h"))), "AAKKQ");
}

TEST(BestFive, WheelPutsTheAceLast) {
    EXPECT_EQ(ranks_of(best_five(hole_of("Ac 2d"), board_of("3s 4h 5d Kc Kh"))), "5432A");
    EXPECT_EQ(ranks_of(best_five(hole_of("6c 2d"), board_of("3s 4h 5d Ac Kh"))), "65432");
}

TEST(BestFive, BoardThatPlaysIgnoresHoleCards) {
    EXPECT_EQ(ranks_of(best_five(hole_of("2c 3d"), board_of("As Ks Qd Jc Th"))), "AKQJT");
}

TEST(BestFive, FlushUsesTheTopFiveOfSixSuited) {
    const auto five = best_five(hole_of("Ks 2s"), board_of("As 9s 7s 4s 3d"));
    EXPECT_EQ(ranks_of(five), "AK974");
    for (const Card& c : five) EXPECT_EQ(c.suit(), Suit::Spades);
}

TEST(BestFive, MatchesTheSevenCardRank) {
    std::mt19937_64 rng(7);
    for (int trial = 0; trial < 2000; ++trial) {
        Deck deck;
        deck.shuffle(rng);
        const std::array<Card, 2> hole{deck.deal(), deck.deal()};
        const std::array<Card, 5> board{deck.deal(), deck.deal(), deck.deal(), deck.deal(), deck.deal()};
        EXPECT_EQ(evaluate5(best_five(hole, board)), evaluate7(hole, board));
    }
}

TEST(KickerDifference, PairDecidedByKicker) {
    const auto board = board_of("Kh 9d 7c 4s 2h");
    const auto a = hole_of("Kc As");
    const auto b = hole_of("Kd Qs");
    const HandRank ra = evaluate7(a, board);
    const HandRank rb = evaluate7(b, board);
    ASSERT_EQ(describe_made_hand(ra), describe_made_hand(rb));
    const auto diff = kicker_difference(ra, best_five(a, board), rb, best_five(b, board));
    ASSERT_TRUE(diff);
    EXPECT_EQ(diff->winner, Rank::Ace);
    EXPECT_EQ(diff->loser, Rank::Queen);
    EXPECT_FALSE(kicker_difference(rb, best_five(b, board), ra, best_five(a, board)));  // Worse hand first
}

TEST(KickerDifference, TripsAndFlushes) {
    const auto trips_board = board_of("Qh Qd 8c 5s 2h");
    const auto t1 = hole_of("Qc Jd");
    const auto t2 = hole_of("Qs Td");
    const auto trips = kicker_difference(evaluate7(t1, trips_board), best_five(t1, trips_board),
                                         evaluate7(t2, trips_board), best_five(t2, trips_board));
    ASSERT_TRUE(trips);
    EXPECT_EQ(trips->winner, Rank::Jack);
    EXPECT_EQ(trips->loser, Rank::Ten);

    const auto flush_board = board_of("Ah 9h 6h 2h Kc");
    const auto f1 = hole_of("Qh 3d");
    const auto f2 = hole_of("Jh Ts");
    const auto flush = kicker_difference(evaluate7(f1, flush_board), best_five(f1, flush_board),
                                         evaluate7(f2, flush_board), best_five(f2, flush_board));
    ASSERT_TRUE(flush);
    EXPECT_EQ(flush->winner, Rank::Queen);
    EXPECT_EQ(flush->loser, Rank::Jack);
}

TEST(KickerDifference, NoneForTiesOrDifferentNames) {
    const auto board = board_of("As Ks Qd Jc Th");
    const auto a = hole_of("2c 3d");
    const auto b = hole_of("4h 5c");
    EXPECT_FALSE(kicker_difference(evaluate7(a, board), best_five(a, board), evaluate7(b, board), best_five(b, board)));

    const auto pair_board = board_of("Kh 9d 7c 4s 2h");
    const auto kings = hole_of("Kc As");
    const auto nines = hole_of("9c As");
    EXPECT_FALSE(kicker_difference(evaluate7(kings, pair_board), best_five(kings, pair_board),
                                   evaluate7(nines, pair_board), best_five(nines, pair_board)));
}

TEST(HandEvaluator, RankNames) {
    EXPECT_EQ(rank_name(Rank::Ace), "Ace");
    EXPECT_EQ(rank_name(Rank::Ten), "Ten");
    EXPECT_EQ(rank_name(Rank::Two), "Two");
}
