#include "range/preflop_matrix.h"

namespace poker {

double class_vs_class_equity(HandClass hero, HandClass villain) {
    constexpr double kScale = 65535.0;
    const int i = hero.index();
    const int j = villain.index();
    // Only i <= j is stored; the reverse matchup is the other side of the same pot.
    if (i <= j) return detail::kPreflopEquity[detail::preflop_triangle_index(i, j)] / kScale;
    return 1.0 - detail::kPreflopEquity[detail::preflop_triangle_index(j, i)] / kScale;
}

int preflop_matrix_rollouts() { return detail::kPreflopEquityRollouts; }

}  // namespace poker
