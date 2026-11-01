#include "engine/pots.h"

#include <algorithm>

namespace poker {

PotBreakdown calculate_pots(const std::array<int, kMaxSeats>& contributed,
                            const std::array<bool, kMaxSeats>& live) {
    std::vector<int> levels;
    for (const int amount : contributed) {
        if (amount > 0) levels.push_back(amount);
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    PotBreakdown result;
    int previous_level = 0;
    int carried = 0;  // Dead chips waiting for a pot, when no pot exists yet

    for (const int level : levels) {
        std::vector<int> eligible;
        int contributors = 0;
        int single_contributor = -1;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            if (contributed[seat] < level) continue;
            ++contributors;
            single_contributor = seat;
            if (live[seat]) eligible.push_back(seat);
        }
        const int layer = (level - previous_level) * contributors;
        previous_level = level;

        if (contributors == 1 && !eligible.empty()) {
            result.refund = Refund{single_contributor, layer};
        } else if (eligible.empty()) {
            if (result.pots.empty()) {
                carried += layer;
            } else {
                result.pots.back().amount += layer;
            }
        } else if (!result.pots.empty() && result.pots.back().eligible == eligible) {
            result.pots.back().amount += layer + carried;
            carried = 0;
        } else {
            result.pots.push_back(Pot{layer + carried, std::move(eligible)});
            carried = 0;
        }
    }
    return result;
}

}  // namespace poker
