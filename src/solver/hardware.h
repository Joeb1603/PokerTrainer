#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "engine/table_types.h"
#include "solver/betting_tree.h"

namespace poker {

// What the machine offers the solver, read once.
struct HardwareInfo {
    unsigned threads = 1;          // std::thread::hardware_concurrency, 0 read as 1
    std::uint64_t total_ram = 0;   // Bytes of physical memory
    std::uint64_t avail_ram = 0;   // Bytes available to a new allocation without swapping
    bool ram_known = true;         // False where the platform gives no memory figures
};

// Windows: GlobalMemoryStatusEx (ullTotalPhys, ullAvailPhys). Linux: MemTotal and
// MemAvailable from /proc/meminfo, which counts reclaimable page cache as available;
// without it, sysconf(_SC_PHYS_PAGES / _SC_AVPHYS_PAGES) times the page size, where
// "available" is only the free pages and so reads low. Elsewhere the memory is
// unknown (ram_known false), which choose_tier treats as the river tier. Container
// memory limits (cgroups) are not read.
HardwareInfo detect_hardware();

// What solver mode can solve, from the least to the most.
enum class SolverTier : std::uint8_t { Unavailable, River, Turn, Flop };

constexpr std::string_view to_string(SolverTier tier) {
    switch (tier) {
        case SolverTier::Unavailable: return "Unavailable";
        case SolverTier::River: return "River";
        case SolverTier::Turn: return "Turn";
        case SolverTier::Flop: return "Flop";
    }
    return "Unavailable";
}

inline constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;

// Unavailable: fewer than 2 threads, or under 1 GB available.
// Flop: at least 8 threads and 8 GB available.
// Turn: at least 4 threads and 4 GB available.
// River: anything else, including unknown memory with 2 or more threads.
SolverTier choose_tier(const HardwareInfo& info);

// Half the available memory, at most 16 GB (256 MB when the memory is unknown,
// which is plenty for river trees).
std::uint64_t solver_memory_budget(const HardwareInfo& info);

// Every thread but one, which stays free for the UI; at least 1.
int solver_threads(const HardwareInfo& info);

// The tree a solve uses.
enum class SolverTree : std::uint8_t { River, Turn, FlopCheckDown, FlopFull };

// Which tree `tier` solves on `street`, or nothing (preflop, or a street the tier
// does not cover):
//
// | Street | Unavailable | River | Turn                  | Flop                                   |
// |--------|-------------|-------|-----------------------|----------------------------------------|
// | River  | -           | River | River                 | River                                  |
// | Turn   | -           | -     | Turn                  | Turn                                   |
// | Flop   | -           | -     | FlopCheckDown         | FlopFull if it fits, else FlopCheckDown |
//
// `flop_full_fits`: whether the full flop tree's estimate is within the memory
// budget and the solve has time for it (see kFlopFullMinBudgetMs). The Turn tier
// gets the approximate flop tree too: it is about the size of a turn tree.
std::optional<SolverTree> tree_for(SolverTier tier, Street street, bool flop_full_fits);

// The shortest time budget worth a full flop tree: the UI's longest. Its iterations
// take 2-4 s on 3-4 threads and it needs about 100 to reach 2% of the pot. Measured
// on 3 threads with heads-up ranges (798 x 790 combos, 5.2 GB): 4 iterations in
// 20 s left it at 237% of the pot, 14 in 60 s at 46%. Shorter budgets get the
// approximate tree, which reached 2% in about 12 s on the same spot.
inline constexpr int kFlopFullMinBudgetMs = 120'000;

BetAbstraction abstraction_for(SolverTree tree);

// "River", "Turn", "Flop (approximate: no river betting)" or "Flop".
std::string_view street_label(SolverTree tree);

// The exploitability, as a % of the pot, at which a solve of this tree stops early:
// 0.3 (river), 0.8 (turn) or 2 (flop).
double target_exploitability_pct(SolverTree tree);

// One line for a tooltip, e.g. "4 threads, 15.6 GB free: solves river and turn,
// flop approximate".
std::string hardware_summary(const HardwareInfo& info, SolverTier tier);

}  // namespace poker
