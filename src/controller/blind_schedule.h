#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/table_types.h"

namespace poker {

// The game a session plays: a freezeout with fixed blinds, or a tournament whose
// blinds rise level by level. Both are six seats, no rebuys; only the blinds differ.
// Pure data: no engine state, no hidden information.

enum class GameMode : std::uint8_t { Freezeout, Tournament };
enum class LevelUnit : std::uint8_t { Hands, Minutes };
enum class BlindPreset : std::uint8_t { Standard, Turbo, Hyper };

inline constexpr std::array<BlindPreset, 3> kBlindPresets{BlindPreset::Standard, BlindPreset::Turbo,
                                                          BlindPreset::Hyper};

std::string_view to_string(GameMode mode);
std::string_view to_string(LevelUnit unit);
std::string_view to_string(BlindPreset preset);
std::optional<GameMode> parse_game_mode(std::string_view text);
std::optional<LevelUnit> parse_level_unit(std::string_view text);
std::optional<BlindPreset> parse_blind_preset(std::string_view text);
std::optional<AnteType> parse_ante_type(std::string_view text);

// Limits the setup screen enforces.
inline constexpr int kMinBigBlind = 2;
inline constexpr int kMaxBigBlind = 10000;
inline constexpr int kMinStartingBb = 10;
inline constexpr int kMaxStartingBb = 500;
inline constexpr int kMaxLevelHands = 100;
inline constexpr int kMaxLevelMinutes = 60;
inline constexpr int kMaxLevels = 40;
inline constexpr int kMaxAnteBigBlinds = 2;  // An ante above twice the big blind is refused
inline constexpr long long kMaxStartingChips = 100'000'000;  // Six stacks stay well inside an int

struct GameSetup {
    GameMode mode = GameMode::Freezeout;
    int big_blind = 20;    // Freezeout's blind, and the tournament's first level
    int starting_bb = 100; // Starting stack in big blinds of the first level

    // Tournament only.
    LevelUnit unit = LevelUnit::Hands;
    int level_length = 10;  // Hands or minutes per level
    BlindPreset preset = BlindPreset::Standard;
    AnteType ante_type = AnteType::BigBlind;
    int ante_from_level = 4;          // 1-based; the presets' antes start here
    std::vector<BlindLevel> levels;   // Editable; empty means "the preset as is"

    int starting_stack() const { return starting_bb * first_level().big_blind; }
    BlindLevel first_level() const;
    // The tournament's levels, the preset's when none were set.
    std::vector<BlindLevel> schedule() const;

    friend bool operator==(const GameSetup&, const GameSetup&) = default;
};

// Today's table: Freezeout, 10/20, 100 BB.
GameSetup default_freezeout();
// Standard levels of 10 hands from 20, 50 BB stacks, a big-blind ante from level 4.
GameSetup default_tournament();

// Half the big blind, rounded down to a tidy amount (10/20, 60/125, 10/25), at least 1.
int small_blind_for(int big_blind);

// Rounds to an amount a dealer would use: 1s below 10, 5s below 100, then 25s,
// 250s and so on (two significant figures at most). Never below 1.
int tidy_chips(double amount);

// The preset's big blinds as multiples of the first.
const std::vector<double>& preset_multiples(BlindPreset preset);

// A preset scaled to `big_blind`: tidy, strictly rising big blinds, small blind
// half the big (rounded down), and antes from `ante_from_level` (see with_antes).
std::vector<BlindLevel> preset_schedule(BlindPreset preset, int big_blind, AnteType ante_type, int ante_from_level);

// Sets every level's ante: a big-blind ante equals the big blind, a per-player ante
// is an eighth of it (tidied, at least 1); levels before `from_level` get none.
std::vector<BlindLevel> with_antes(std::vector<BlindLevel> levels, AnteType type, int from_level);

// Every level's blinds and ante times `factor`, tidied, keeping the big blinds
// strictly rising and each small blind at its old share of the big.
std::vector<BlindLevel> scale_schedule(const std::vector<BlindLevel>& levels, double factor);

// Why the setup cannot start, or nullopt when it can.
std::optional<std::string> validate(const GameSetup& setup);

// A short description, e.g. "Tournament: 10/20, 50 BB, Standard, 10 hands per level".
std::string describe(const GameSetup& setup);

// Tracks which level a tournament is on. A level changes only when a hand starts,
// from the hands started or the time played so far, so a long pause can skip
// levels. The last level lasts for ever.
class TournamentClock {
public:
    TournamentClock() = default;
    TournamentClock(std::vector<BlindLevel> levels, LevelUnit unit, int level_length);

    // Call before each hand; returns that hand's level.
    const BlindLevel& on_hand_started();

    // Time played, ignored while paused. Each call counts at most kMaxStepMs, so a
    // frame that stalled (a window drag) does not jump the clock.
    static constexpr double kMaxStepMs = 1000.0;
    void advance_ms(double ms);
    void set_paused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }

    int level_index() const { return index_; }  // 0-based, of the latest hand
    bool level_changed() const { return changed_; }  // The latest hand began a new level
    const BlindLevel& current_level() const;
    std::optional<BlindLevel> next_level() const;
    int hands_started() const { return hands_; }
    double elapsed_ms() const { return elapsed_ms_; }
    LevelUnit unit() const { return unit_; }
    int level_length() const { return length_; }

    // Hands mode: hands still to start before the next level (0 = the next hand).
    int hands_until_next_level() const;
    // Minutes mode: time left in this level; at or below 0, the next hand moves up.
    double ms_until_next_level() const;

private:
    int index_for_now() const;

    std::vector<BlindLevel> levels_{BlindLevel{}};
    LevelUnit unit_ = LevelUnit::Hands;
    int length_ = 10;
    int hands_ = 0;
    double elapsed_ms_ = 0.0;
    bool paused_ = false;
    int index_ = 0;
    bool changed_ = false;
};

}  // namespace poker
