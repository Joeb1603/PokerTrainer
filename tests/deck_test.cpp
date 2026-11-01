#include <gtest/gtest.h>

#include <array>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include "core/deck.h"

using namespace poker;

namespace {

std::vector<Card> deal_all(Deck deck) {
    std::vector<Card> cards;
    while (deck.remaining() > 0) cards.push_back(deck.deal());
    return cards;
}

}  // namespace

TEST(Deck, NewDeckHas52UniqueCards) {
    const auto cards = deal_all(Deck{});
    EXPECT_EQ(cards.size(), 52u);
    EXPECT_EQ(std::set<Card>(cards.begin(), cards.end()).size(), 52u);
}

TEST(Deck, DealOnEmptyThrows) {
    Deck deck;
    for (int i = 0; i < kDeckSize; ++i) deck.deal();
    EXPECT_EQ(deck.remaining(), 0u);
    EXPECT_THROW(deck.deal(), std::out_of_range);
}

TEST(Deck, ShufflePreservesCards) {
    Deck deck;
    std::mt19937_64 rng(42);
    deck.shuffle(rng);
    const auto cards = deal_all(deck);
    EXPECT_EQ(std::set<Card>(cards.begin(), cards.end()).size(), 52u);
}

TEST(Deck, SameSeedGivesSameOrderAndDifferentSeedDiffers) {
    const auto shuffled = [](std::uint64_t seed) {
        Deck deck;
        std::mt19937_64 rng(seed);
        deck.shuffle(rng);
        return deal_all(deck);
    };
    EXPECT_EQ(shuffled(7), shuffled(7));
    EXPECT_NE(shuffled(7), shuffled(8));
    EXPECT_NE(shuffled(7), deal_all(Deck{}));
}

TEST(Deck, ShuffleLeavesDealtCardsOut) {
    Deck deck;
    const Card first = deck.deal();
    std::mt19937_64 rng(1);
    deck.shuffle(rng);
    EXPECT_EQ(deck.remaining(), 51u);
    const auto rest = deal_all(deck);
    EXPECT_EQ(std::set<Card>(rest.begin(), rest.end()).count(first), 0u);
    EXPECT_EQ(std::set<Card>(rest.begin(), rest.end()).size(), 51u);
}

TEST(Deck, ExcludedCardsAreNeverDealt) {
    const std::array excluded{*Card::parse("As"), *Card::parse("Kd"), *Card::parse("2c")};
    Deck deck(excluded);
    EXPECT_EQ(deck.remaining(), 49u);

    std::mt19937_64 rng(3);
    deck.shuffle(rng);
    const auto cards = deal_all(deck);
    const std::set<Card> dealt(cards.begin(), cards.end());
    EXPECT_EQ(dealt.size(), 49u);
    for (const Card card : excluded) EXPECT_EQ(dealt.count(card), 0u);
}

TEST(Deck, DuplicateExclusionsCountOnce) {
    const std::array excluded{*Card::parse("As"), *Card::parse("As")};
    EXPECT_EQ(Deck(excluded).remaining(), 51u);
}

TEST(Deck, DealRandomGivesEveryCardOnceThenThrows) {
    Deck deck;
    std::mt19937_64 rng(5);
    std::set<Card> dealt;
    for (int i = 0; i < kDeckSize; ++i) dealt.insert(deck.deal_random(rng));
    EXPECT_EQ(dealt.size(), 52u);
    EXPECT_EQ(deck.remaining(), 0u);
    EXPECT_THROW(deck.deal_random(rng), std::out_of_range);
}

// 51 degrees of freedom: p = 0.001 critical value is about 87.
TEST(Deck, DealRandomIsUniformFromAnOrderedDeck) {
    constexpr int kTrials = 260000;
    std::array<int, kDeckSize> first_counts{};
    std::array<int, kDeckSize> second_counts{};
    std::mt19937_64 rng(777);
    for (int t = 0; t < kTrials; ++t) {
        Deck deck;
        ++first_counts[deck.deal_random(rng).id];
        ++second_counts[deck.deal_random(rng).id];
    }

    const double expected = static_cast<double>(kTrials) / kDeckSize;
    const auto chi_squared = [expected](const std::array<int, kDeckSize>& counts) {
        double total = 0.0;
        for (const int count : counts) total += (count - expected) * (count - expected) / expected;
        return total;
    };
    EXPECT_LT(chi_squared(first_counts), 87.0);
    EXPECT_LT(chi_squared(second_counts), 87.0);
}

// Chi-squared test on which card lands on top. The seed is fixed, so this is
// deterministic. 51 degrees of freedom: p = 0.001 critical value is about 87.
TEST(Deck, ShuffleIsUniform) {
    constexpr int kTrials = 260000;
    std::array<int, kDeckSize> top_counts{};
    std::mt19937_64 rng(12345);
    for (int t = 0; t < kTrials; ++t) {
        Deck deck;
        deck.shuffle(rng);
        ++top_counts[deck.deal().id];
    }

    const double expected = static_cast<double>(kTrials) / kDeckSize;
    double chi_squared = 0.0;
    for (const int count : top_counts) {
        const double diff = count - expected;
        chi_squared += diff * diff / expected;
    }
    EXPECT_LT(chi_squared, 87.0);
}
