#include "engine/pots.h"

#include <algorithm>
#include <limits>
#include <utility>

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

LivePots calculate_live_pots(const std::array<int, kMaxSeats>& contributed,
                             const std::array<SeatStatus, kMaxSeats>& statuses) {
    LivePots result;
    const auto is_live = [&statuses](int seat) {
        return statuses[seat] == SeatStatus::Active || statuses[seat] == SeatStatus::AllIn;
    };

    // Chips above what anyone else has put in are unmatched, unless their owner folded.
    std::array<int, kMaxSeats> counted = contributed;
    int highest_seat = 0;
    int second_highest = 0;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        result.total += contributed[seat];
        if (contributed[seat] > contributed[highest_seat]) highest_seat = seat;
    }
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (seat != highest_seat) second_highest = std::max(second_highest, contributed[seat]);
    }
    if (is_live(highest_seat)) {
        result.uncalled = contributed[highest_seat] - second_highest;
        counted[highest_seat] = second_highest;
    }

    std::vector<int> levels;
    for (int seat = 0; seat < kMaxSeats; ++seat) {
        if (statuses[seat] == SeatStatus::AllIn && counted[seat] > 0) levels.push_back(counted[seat]);
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    constexpr int kNoCap = std::numeric_limits<int>::max();
    levels.push_back(kNoCap);  // Whatever sits above the highest all-in

    int previous = 0;
    for (const int level : levels) {
        int amount = 0;
        std::vector<int> eligible;
        for (int seat = 0; seat < kMaxSeats; ++seat) {
            amount += std::min(counted[seat], level) - std::min(counted[seat], previous);
            const bool can_win = statuses[seat] == SeatStatus::Active ||
                                 (statuses[seat] == SeatStatus::AllIn && counted[seat] >= level);
            if (can_win) eligible.push_back(seat);
        }
        previous = level;
        if (amount == 0) continue;

        const bool merges = eligible.empty() || (!result.pots.empty() && result.pots.back().eligible == eligible);
        if (merges && !result.pots.empty()) {
            // Dead chips stay with the pot below; matching layers are one pot.
            result.pots.back().amount += amount;
        } else {
            result.pots.push_back(Pot{amount, std::move(eligible)});
        }
    }
    return result;
}

}  // namespace poker
