#include "solver/hardware.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // Guarded: a /DNOMINMAX on the command line would make this C4005
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

namespace poker {
namespace {

#if !defined(_WIN32) && defined(__linux__)
// A "Key:   12345 kB" line of /proc/meminfo, in bytes.
std::optional<std::uint64_t> meminfo_bytes(const std::string& text, std::string_view key) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.compare(0, key.size(), key) != 0 || line.size() <= key.size() || line[key.size()] != ':') continue;
        std::istringstream fields(line.substr(key.size() + 1));
        std::uint64_t kilobytes = 0;
        if (fields >> kilobytes) return kilobytes * 1024;
    }
    return std::nullopt;
}
#endif

std::string gigabytes(std::uint64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f GB", static_cast<double>(bytes) / static_cast<double>(kGiB));
    return buffer;
}

}  // namespace

HardwareInfo detect_hardware() {
    HardwareInfo info;
    info.threads = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        info.total_ram = status.ullTotalPhys;
        info.avail_ram = status.ullAvailPhys;
    } else {
        info.ram_known = false;
    }
#elif defined(__linux__)
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> avail;
    if (std::ifstream in("/proc/meminfo"); in) {
        std::ostringstream text;
        text << in.rdbuf();
        total = meminfo_bytes(text.str(), "MemTotal");
        avail = meminfo_bytes(text.str(), "MemAvailable");
    }
    const long page = sysconf(_SC_PAGE_SIZE);
    if (!total && page > 0) {
        const long pages = sysconf(_SC_PHYS_PAGES);
        if (pages > 0) total = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    }
    if (!avail && page > 0) {
        const long pages = sysconf(_SC_AVPHYS_PAGES);
        if (pages > 0) avail = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    }
    if (total && avail) {
        info.total_ram = *total;
        info.avail_ram = std::min(*avail, *total);
    } else {
        info.ram_known = false;
    }
#else
    info.ram_known = false;
#endif
    return info;
}

SolverTier choose_tier(const HardwareInfo& info) {
    if (info.threads < 2) return SolverTier::Unavailable;
    if (!info.ram_known) return SolverTier::River;
    if (info.avail_ram < kGiB) return SolverTier::Unavailable;
    if (info.threads >= 8 && info.avail_ram >= 8 * kGiB) return SolverTier::Flop;
    if (info.threads >= 4 && info.avail_ram >= 4 * kGiB) return SolverTier::Turn;
    return SolverTier::River;
}

std::uint64_t solver_memory_budget(const HardwareInfo& info) {
    if (!info.ram_known) return 256ull << 20;
    return std::min(info.avail_ram / 2, 16 * kGiB);
}

int solver_threads(const HardwareInfo& info) { return static_cast<int>(info.threads > 1 ? info.threads - 1 : 1); }

std::optional<SolverTree> tree_for(SolverTier tier, Street street, bool flop_full_fits) {
    switch (street) {
        case Street::River:
            if (tier >= SolverTier::River) return SolverTree::River;
            break;
        case Street::Turn:
            if (tier >= SolverTier::Turn) return SolverTree::Turn;
            break;
        case Street::Flop:
            if (tier == SolverTier::Flop && flop_full_fits) return SolverTree::FlopFull;
            if (tier >= SolverTier::Turn) return SolverTree::FlopCheckDown;
            break;
        case Street::Preflop:
        case Street::Showdown: break;
    }
    return std::nullopt;
}

BetAbstraction abstraction_for(SolverTree tree) {
    switch (tree) {
        case SolverTree::River: return river_default_abstraction();
        case SolverTree::Turn: return turn_tier_abstraction();
        case SolverTree::FlopCheckDown: return flop_checkdown_abstraction();
        case SolverTree::FlopFull: return flop_full_abstraction();
    }
    return river_default_abstraction();
}

std::string_view street_label(SolverTree tree) {
    switch (tree) {
        case SolverTree::River: return "River";
        case SolverTree::Turn: return "Turn";
        case SolverTree::FlopCheckDown: return "Flop (approximate: no river betting)";
        case SolverTree::FlopFull: return "Flop";
    }
    return "River";
}

double target_exploitability_pct(SolverTree tree) {
    switch (tree) {
        case SolverTree::River: return 0.3;
        case SolverTree::Turn: return 0.8;
        case SolverTree::FlopCheckDown:
        case SolverTree::FlopFull: return 2.0;
    }
    return 0.3;
}

std::string hardware_summary(const HardwareInfo& info, SolverTier tier) {
    std::string text = std::to_string(info.threads) + (info.threads == 1 ? " thread, " : " threads, ");
    text += info.ram_known ? gigabytes(info.avail_ram) + " free: " : std::string("memory unknown: ");
    switch (tier) {
        case SolverTier::Unavailable:
            text += "solver unavailable (needs 2 threads and 1 GB free)";
            break;
        case SolverTier::River: text += "solves the river only"; break;
        case SolverTier::Turn: text += "solves river and turn, flop approximate"; break;
        case SolverTier::Flop:
            text += "solves river, turn and flop (full flop with a budget of " +
                    std::to_string(kFlopFullMinBudgetMs / 1000) + " s or more, within " +
                    gigabytes(solver_memory_budget(info)) + "; approximate otherwise)";
            break;
    }
    return text;
}

}  // namespace poker
