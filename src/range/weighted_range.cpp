#include "range/weighted_range.h"

#include <algorithm>
#include <cstddef>

namespace poker {

WeightedRange WeightedRange::uniform() {
    WeightedRange range;
    range.weights_.fill(1.0f);
    return range;
}

WeightedRange WeightedRange::from_hand_range(const HandRange& range, float weight) {
    WeightedRange result;
    for (int combo = 0; combo < kComboCount; ++combo) {
        if (range.contains(combo_class(combo))) result.set(combo, weight);
    }
    return result;
}

void WeightedRange::set_class(HandClass hand, float weight) {
    for (const std::uint16_t combo : class_combos(hand).view()) weights_[combo] = weight;
}

void WeightedRange::remove_cards(CardMask cards) {
    for (int combo = 0; combo < kComboCount; ++combo) {
        if ((combo_mask(combo) & cards) != 0) set(combo, 0.0f);
    }
}

double WeightedRange::total() const {
    double sum = 0.0;
    for (const float w : weights_) sum += w;
    return sum;
}

double WeightedRange::class_mass(HandClass hand) const {
    double sum = 0.0;
    for (const std::uint16_t combo : class_combos(hand).view()) sum += weights_[combo];
    return sum;
}

WeightedRange& WeightedRange::multiply(const WeightedRange& other) {
    for (std::size_t i = 0; i < weights_.size(); ++i) weights_[i] *= other.weights_[i];
    return *this;
}

double WeightedRange::share_of(const WeightedRange& subset) const {
    double inside = 0.0;
    double all = 0.0;
    for (std::size_t i = 0; i < weights_.size(); ++i) {
        const double w = weights_[i];
        inside += w * std::clamp(static_cast<double>(subset.weights_[i]), 0.0, 1.0);
        all += w;
    }
    return all > 0.0 ? inside / all : 0.0;
}

}  // namespace poker
