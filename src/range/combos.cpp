#include "range/combos.h"

#include <cstddef>

namespace poker {
namespace {

struct ComboTables {
    std::array<std::array<Card, 2>, kComboCount> cards{};
    std::array<HandClass, kComboCount> hand_class{};
    std::array<CardMask, kComboCount> mask{};
    std::array<ClassCombos, kHandClassCount> by_class{};
};

ComboTables make_tables() {
    ComboTables tables;
    for (int high = 1; high < kDeckSize; ++high) {
        for (int low = 0; low < high; ++low) {
            const Card a(low);
            const Card b(high);
            const auto combo = static_cast<std::size_t>(combo_index(a, b));
            const HandClass hand = HandClass::from_cards(a, b);
            tables.cards[combo] = {a, b};
            tables.hand_class[combo] = hand;
            tables.mask[combo] = card_mask(a) | card_mask(b);
        }
    }
    // A second pass in index order keeps each class's list sorted.
    for (std::size_t combo = 0; combo < tables.hand_class.size(); ++combo) {
        ClassCombos& entry = tables.by_class[static_cast<std::size_t>(tables.hand_class[combo].index())];
        entry.combos[static_cast<std::size_t>(entry.count++)] = static_cast<std::uint16_t>(combo);
    }
    return tables;
}

// Built once at run time rather than as a constexpr table: MSVC's default
// constexpr step limit is far below what filling these tables takes.
const ComboTables& tables() {
    static const ComboTables built = make_tables();
    return built;
}

static_assert(combo_index(Card(0), Card(1)) == 0);
static_assert(combo_index(Card(51), Card(50)) == kComboCount - 1);

}  // namespace

std::array<Card, 2> combo_cards(int combo) { return tables().cards[static_cast<std::size_t>(combo)]; }

HandClass combo_class(int combo) { return tables().hand_class[static_cast<std::size_t>(combo)]; }

CardMask combo_mask(int combo) { return tables().mask[static_cast<std::size_t>(combo)]; }

const ClassCombos& class_combos(HandClass hand) { return tables().by_class[static_cast<std::size_t>(hand.index())]; }

}  // namespace poker
