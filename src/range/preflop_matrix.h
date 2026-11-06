#pragma once

#include <cstdint>

#include "strategy/preflop_charts.h"

namespace poker {

// Hero's all-in preflop equity (wins plus half of ties) with `hero` against
// `villain`, averaged uniformly over every pair of concrete combos of the two
// classes that share no card. Read from a table that tools/gen_preflop_tables.cpp
// fills by Monte Carlo, so each value carries a standard error of about
// 0.5 / sqrt(preflop_matrix_rollouts()). class_vs_class_equity(a, b) +
// class_vs_class_equity(b, a) is 1 up to rounding, except for a == b, which is
// measured on its own and lands near 0.5.
double class_vs_class_equity(HandClass hero, HandClass villain);

// Monte Carlo rollouts behind each table entry.
int preflop_matrix_rollouts();

namespace detail {

// Packed upper triangle with diagonal, by HandClass::index(): entry (i, j) with
// i <= j sits at i * 169 - i * (i - 1) / 2 + (j - i), holding equity * 65535.
inline constexpr int kPreflopTriangleSize = kHandClassCount * (kHandClassCount + 1) / 2;

constexpr int preflop_triangle_index(int i, int j) { return i * kHandClassCount - i * (i - 1) / 2 + (j - i); }

static_assert(preflop_triangle_index(kHandClassCount - 1, kHandClassCount - 1) == kPreflopTriangleSize - 1);

// Defined in range/generated/preflop_equity_data.cpp.
extern const std::uint16_t kPreflopEquity[kPreflopTriangleSize];
extern const int kPreflopEquityRollouts;

}  // namespace detail
}  // namespace poker
