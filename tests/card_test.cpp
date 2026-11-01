#include <gtest/gtest.h>

#include <set>

#include "core/card.h"

using namespace poker;

TEST(Card, IdEncodingMatchesPhevaluator) {
    EXPECT_EQ(Card(Rank::Two, Suit::Clubs).id, 0);
    EXPECT_EQ(Card(Rank::Two, Suit::Spades).id, 3);
    EXPECT_EQ(Card(Rank::Three, Suit::Clubs).id, 4);
    EXPECT_EQ(Card(Rank::Ten, Suit::Hearts).id, 34);
    EXPECT_EQ(Card(Rank::Ace, Suit::Spades).id, 51);
}

TEST(Card, RankAndSuitRoundTrip) {
    for (int r = 0; r < kRankCount; ++r) {
        for (int s = 0; s < kSuitCount; ++s) {
            const Card card(static_cast<Rank>(r), static_cast<Suit>(s));
            EXPECT_EQ(static_cast<int>(card.rank()), r);
            EXPECT_EQ(static_cast<int>(card.suit()), s);
        }
    }
}

TEST(Card, StringRoundTripCoversAllCardsUniquely) {
    std::set<std::string> names;
    for (int id = 0; id < kDeckSize; ++id) {
        const Card card(id);
        const std::string name = card.to_string();
        names.insert(name);
        EXPECT_EQ(Card::parse(name), card) << name;
    }
    EXPECT_EQ(names.size(), 52u);
}

TEST(Card, ToStringFormat) {
    EXPECT_EQ(Card(Rank::Ace, Suit::Spades).to_string(), "As");
    EXPECT_EQ(Card(Rank::Ten, Suit::Diamonds).to_string(), "Td");
    EXPECT_EQ(Card(Rank::Two, Suit::Clubs).to_string(), "2c");
}

TEST(Card, ParseIsCaseInsensitive) {
    EXPECT_EQ(Card::parse("as"), Card(Rank::Ace, Suit::Spades));
    EXPECT_EQ(Card::parse("AS"), Card(Rank::Ace, Suit::Spades));
    EXPECT_EQ(Card::parse("tD"), Card(Rank::Ten, Suit::Diamonds));
}

TEST(Card, ParseRejectsInvalidInput) {
    for (const char* bad : {"", "A", "Asd", "1s", "Ax", "10s", "s2", " A"}) {
        EXPECT_FALSE(Card::parse(bad).has_value()) << '"' << bad << '"';
    }
}
