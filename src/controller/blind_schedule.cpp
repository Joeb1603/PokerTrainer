#include "controller/blind_schedule.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace poker {
namespace {

// The tidy step for an amount: 1 below 10, 5 below 100, then a quarter of the
// leading power of ten (25, 250, 2500, ...).
int tidy_step(double amount) {
    if (amount < 10.0) return 1;
    if (amount < 100.0) return 5;
    double power = 100.0;
    while (amount >= power * 10.0) power *= 10.0;
    return static_cast<int>(power / 4.0);
}

int next_tidy_above(int amount) { return amount + tidy_step(amount); }

constexpr int kMaxLevelBigBlind = 100'000'000;

}  // namespace

std::string_view to_string(GameMode mode) { return mode == GameMode::Tournament ? "Tournament" : "Freezeout"; }

std::string_view to_string(LevelUnit unit) { return unit == LevelUnit::Minutes ? "Minutes" : "Hands"; }

std::string_view to_string(BlindPreset preset) {
    switch (preset) {
        case BlindPreset::Turbo: return "Turbo";
        case BlindPreset::Hyper: return "Hyper";
        case BlindPreset::Standard: break;
    }
    return "Standard";
}

std::optional<GameMode> parse_game_mode(std::string_view text) {
    if (text == "freezeout" || text == "Freezeout") return GameMode::Freezeout;
    if (text == "tournament" || text == "Tournament") return GameMode::Tournament;
    return std::nullopt;
}

std::optional<LevelUnit> parse_level_unit(std::string_view text) {
    if (text == "hands" || text == "Hands") return LevelUnit::Hands;
    if (text == "minutes" || text == "Minutes") return LevelUnit::Minutes;
    return std::nullopt;
}

std::optional<BlindPreset> parse_blind_preset(std::string_view text) {
    const auto same_letters = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    for (const BlindPreset preset : kBlindPresets) {
        const std::string_view name = to_string(preset);
        if (std::equal(text.begin(), text.end(), name.begin(), name.end(), same_letters)) return preset;
    }
    return std::nullopt;
}

std::optional<AnteType> parse_ante_type(std::string_view text) {
    if (text == "none" || text == "None") return AnteType::None;
    if (text == "player" || text == "PerPlayer") return AnteType::PerPlayer;
    if (text == "bb" || text == "BigBlind") return AnteType::BigBlind;
    return std::nullopt;
}

BlindLevel GameSetup::first_level() const {
    if (mode == GameMode::Tournament && !levels.empty()) return levels.front();
    const int bb = std::max(1, big_blind);
    const BlindLevel plain{small_blind_for(bb), bb, 0, AnteType::None};
    if (mode == GameMode::Freezeout) return plain;
    return with_antes({plain}, ante_type, ante_from_level).front();  // The preset's first level
}

std::vector<BlindLevel> GameSetup::schedule() const {
    if (!levels.empty()) return levels;
    return preset_schedule(preset, big_blind, ante_type, ante_from_level);
}

GameSetup default_freezeout() { return GameSetup{}; }

GameSetup default_tournament() {
    GameSetup setup;
    setup.mode = GameMode::Tournament;
    setup.big_blind = 20;
    setup.starting_bb = 50;
    setup.unit = LevelUnit::Hands;
    setup.level_length = 10;
    setup.preset = BlindPreset::Standard;
    setup.ante_type = AnteType::BigBlind;
    setup.ante_from_level = 4;
    return setup;
}

int small_blind_for(int big_blind) {
    const double half = big_blind / 2.0;
    const int step = tidy_step(half);
    return std::max(1, static_cast<int>(std::floor(half / step)) * step);
}

int tidy_chips(double amount) {
    if (!(amount >= 1.0)) return 1;
    const int step = tidy_step(amount);
    const double rounded = std::round(amount / step) * step;
    return std::max(1, static_cast<int>(std::min(rounded, static_cast<double>(kMaxLevelBigBlind))));
}

const std::vector<double>& preset_multiples(BlindPreset preset) {
    static const std::vector<double> standard{1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 80, 100};
    static const std::vector<double> turbo{1, 2, 3, 4, 6, 8, 12, 16, 25, 35, 50, 75, 100};
    static const std::vector<double> hyper{1, 2, 4, 6, 10, 15, 25, 40, 60, 100};
    switch (preset) {
        case BlindPreset::Turbo: return turbo;
        case BlindPreset::Hyper: return hyper;
        case BlindPreset::Standard: break;
    }
    return standard;
}

std::vector<BlindLevel> preset_schedule(BlindPreset preset, int big_blind, AnteType ante_type, int ante_from_level) {
    std::vector<BlindLevel> levels;
    int previous = 0;
    for (const double multiple : preset_multiples(preset)) {
        int bb = levels.empty() ? std::max(1, big_blind) : tidy_chips(big_blind * multiple);
        if (bb <= previous) bb = next_tidy_above(previous);
        levels.push_back(BlindLevel{small_blind_for(bb), bb, 0, AnteType::None});
        previous = bb;
    }
    return with_antes(std::move(levels), ante_type, ante_from_level);
}

std::vector<BlindLevel> with_antes(std::vector<BlindLevel> levels, AnteType type, int from_level) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
        BlindLevel& level = levels[i];
        const bool on = type != AnteType::None && static_cast<int>(i) + 1 >= from_level;
        level.ante_type = on ? type : AnteType::None;
        if (!on) {
            level.ante = 0;
        } else if (type == AnteType::BigBlind) {
            level.ante = level.big_blind;
        } else {
            level.ante = std::max(1, tidy_chips(level.big_blind / 8.0));
        }
    }
    return levels;
}

std::vector<BlindLevel> scale_schedule(const std::vector<BlindLevel>& levels, double factor) {
    std::vector<BlindLevel> out;
    int previous = 0;
    for (const BlindLevel& level : levels) {
        int bb = tidy_chips(level.big_blind * factor);
        if (bb <= previous) bb = next_tidy_above(previous);
        const double share = level.big_blind > 0 ? static_cast<double>(level.small_blind) / level.big_blind : 0.5;
        int sb = share >= 0.5 ? small_blind_for(bb) : tidy_chips(bb * share);
        sb = std::clamp(sb, 1, bb);
        const int ante = level.ante > 0 ? tidy_chips(level.ante * factor) : 0;
        out.push_back(BlindLevel{sb, bb, ante, ante > 0 ? level.ante_type : AnteType::None});
        previous = bb;
    }
    return out;
}

std::optional<std::string> validate(const GameSetup& setup) {
    if (setup.big_blind < kMinBigBlind || setup.big_blind > kMaxBigBlind) {
        return "Big blind must be " + std::to_string(kMinBigBlind) + " to " + std::to_string(kMaxBigBlind) + " chips.";
    }
    if (setup.starting_bb < kMinStartingBb || setup.starting_bb > kMaxStartingBb) {
        return "Starting stack must be " + std::to_string(kMinStartingBb) + " to " + std::to_string(kMaxStartingBb) +
               " big blinds.";
    }
    if (setup.mode == GameMode::Freezeout) return std::nullopt;

    const int max_length = setup.unit == LevelUnit::Hands ? kMaxLevelHands : kMaxLevelMinutes;
    if (setup.level_length < 1 || setup.level_length > max_length) {
        return "Levels must last 1 to " + std::to_string(max_length) +
               (setup.unit == LevelUnit::Hands ? " hands." : " minutes.");
    }
    const std::vector<BlindLevel> levels = setup.schedule();
    if (levels.empty()) return "The tournament needs at least one level.";
    if (static_cast<long long>(setup.starting_bb) * std::max(0, levels.front().big_blind) > kMaxStartingChips) {
        return "Starting stacks above " + std::to_string(kMaxStartingChips) + " chips are not supported.";
    }
    if (static_cast<int>(levels.size()) > kMaxLevels) return "At most " + std::to_string(kMaxLevels) + " levels.";
    int previous = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const BlindLevel& level = levels[i];
        const std::string name = "Level " + std::to_string(i + 1) + ": ";
        if (level.small_blind < 1 || level.small_blind > level.big_blind) {
            return name + "the small blind must be 1 to the big blind.";
        }
        if (level.big_blind > kMaxLevelBigBlind) return name + "the big blind is too large.";
        if (level.big_blind < previous) return name + "blinds cannot go down.";
        if (level.ante < 0 || level.ante > kMaxAnteBigBlinds * level.big_blind) {
            return name + "the ante must be 0 to " + std::to_string(kMaxAnteBigBlinds) + " big blinds.";
        }
        if (level.ante > 0 && level.ante_type == AnteType::None) return name + "an ante needs a type.";
        previous = level.big_blind;
    }
    return std::nullopt;
}

std::string describe(const GameSetup& setup) {
    const BlindLevel first = setup.first_level();
    std::string text = std::string(to_string(setup.mode)) + ": " + std::to_string(first.small_blind) + "/" +
                       std::to_string(first.big_blind) + ", " + std::to_string(setup.starting_bb) + " BB";
    if (setup.mode == GameMode::Freezeout) return text;
    text += ", " + std::string(setup.levels.empty() ? to_string(setup.preset) : "custom") + " levels of " +
            std::to_string(setup.level_length) + (setup.unit == LevelUnit::Hands ? " hands" : " min");
    return text;
}

TournamentClock::TournamentClock(std::vector<BlindLevel> levels, LevelUnit unit, int level_length)
    : levels_(std::move(levels)), unit_(unit), length_(std::max(1, level_length)) {
    if (levels_.empty()) levels_.push_back(BlindLevel{});
}

int TournamentClock::index_for_now() const {
    long long index = 0;
    if (unit_ == LevelUnit::Hands) {
        index = std::max(0, hands_ - 1) / length_;
    } else {
        index = static_cast<long long>(std::floor(elapsed_ms_ / (length_ * 60'000.0)));
    }
    return static_cast<int>(std::min<long long>(index, static_cast<long long>(levels_.size()) - 1));
}

const BlindLevel& TournamentClock::on_hand_started() {
    ++hands_;
    const int index = index_for_now();
    changed_ = hands_ > 1 && index != index_;
    index_ = index;
    return levels_[static_cast<std::size_t>(index_)];
}

void TournamentClock::advance_ms(double ms) {
    if (paused_ || !(ms > 0.0)) return;
    elapsed_ms_ += std::min(ms, kMaxStepMs);
}

const BlindLevel& TournamentClock::current_level() const { return levels_[static_cast<std::size_t>(index_)]; }

std::optional<BlindLevel> TournamentClock::next_level() const {
    const auto next = static_cast<std::size_t>(index_) + 1;
    if (next >= levels_.size()) return std::nullopt;
    return levels_[next];
}

int TournamentClock::hands_until_next_level() const {
    if (unit_ != LevelUnit::Hands || !next_level()) return 0;
    return std::max(0, (index_ + 1) * length_ - hands_);
}

double TournamentClock::ms_until_next_level() const {
    if (unit_ != LevelUnit::Minutes || !next_level()) return 0.0;
    return (index_ + 1) * length_ * 60'000.0 - elapsed_ms_;
}

}  // namespace poker
