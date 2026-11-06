#pragma once

#include <array>

#include "range/combos.h"
#include "strategy/preflop_charts.h"

namespace poker {

// A weight for each of the 1326 combos, usually in [0, 1]: how much of a combo a
// player holds in a given spot. Sums run in double over index order, so results do
// not depend on how the range was built.
class WeightedRange {
public:
    // Every combo at zero weight.
    WeightedRange() = default;

    // Every combo at weight 1, which totals 1326.
    static WeightedRange uniform();

    // Each combo of a class in `range` at `weight`, all others at zero.
    static WeightedRange from_hand_range(const HandRange& range, float weight = 1.0f);

    float weight(int combo) const { return weights_[static_cast<std::size_t>(combo)]; }
    void set(int combo, float weight) { weights_[static_cast<std::size_t>(combo)] = weight; }

    // Sets every combo of `hand` to `weight`.
    void set_class(HandClass hand, float weight);

    const std::array<float, kComboCount>& weights() const { return weights_; }

    // Zeroes every combo holding a card in `cards`, such as known hole or board cards.
    void remove_cards(CardMask cards);

    // The summed weight: the number of combos, counting fractional weights.
    double total() const;

    // The summed weight of the combos of one class.
    double class_mass(HandClass hand) const;

    // Combo-wise product, for narrowing a range by an action's likelihood.
    WeightedRange& multiply(const WeightedRange& other);

    // The share of this range's mass that lies in `subset`, read as a membership
    // filter: sum(this[i] * clamp(subset[i], 0, 1)) / total(). For example, the part
    // of a calling range that is a set. 0 when this range is empty.
    double share_of(const WeightedRange& subset) const;

private:
    std::array<float, kComboCount> weights_{};
};

}  // namespace poker
