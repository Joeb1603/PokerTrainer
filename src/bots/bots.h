#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "bots/bot_strategy.h"
#include "bots/gto_heuristic_engine.h"
#include "equity/equity.h"

namespace poker {

// A bot driven directly by one profile: Nit, Loose and GTO.
class ProfileBot : public IBotStrategy {
public:
    ProfileBot(PersonalityProfile profile, std::uint64_t seed) : engine_(profile), rng_(seed) {}

    void begin_hand(int) override {}
    Action decide(const PlayerView& view) override { return engine_.decide(view, rng_); }
    std::string_view name() const override { return engine_.profile().name; }

private:
    GtoHeuristicEngine engine_;
    std::mt19937_64 rng_;
};

class NitBot final : public ProfileBot {
public:
    explicit NitBot(std::uint64_t seed, int rollouts = kDefaultRollouts) : ProfileBot(nit_profile(rollouts), seed) {}
};

class LooseBot final : public ProfileBot {
public:
    explicit LooseBot(std::uint64_t seed, int rollouts = kDefaultRollouts)
        : ProfileBot(loose_profile(rollouts), seed) {}
};

class GtoBot final : public ProfileBot {
public:
    explicit GtoBot(std::uint64_t seed, int rollouts = kDefaultRollouts) : ProfileBot(gto_profile(rollouts), seed) {}
};

inline constexpr double kWildOverrideProbability = 0.20;
inline constexpr double kWildOverbetFraction = 2.0;

// Takes the exact GTO decision, then with kWildOverrideProbability swaps it for
// an uncautious one. The inner GtoBot gets the bot's own seed and the override
// rolls have a separate stream, so a GtoBot built with the same seed makes the
// same underlying decisions, untouched by the rolls.
class WildGtoBot final : public IBotStrategy {
public:
    explicit WildGtoBot(std::uint64_t seed, int rollouts = kDefaultRollouts);

    void begin_hand(int hand_number) override { gto_.begin_hand(hand_number); }
    Action decide(const PlayerView& view) override;
    std::string_view name() const override { return "WildGTO"; }

    // Whether the latest roll came up. The action can still equal the GTO one when
    // the rules leave nothing else to do.
    bool last_roll_overrode() const { return last_roll_overrode_; }

private:
    GtoBot gto_;
    std::mt19937_64 rng_;
    bool last_roll_overrode_ = false;
};

// Picks one of the four other personalities at the start of each hand and plays
// it. name() stays "Shifter"; only hand_log_label() names the active profile.
class ShifterBot final : public IBotStrategy {
public:
    explicit ShifterBot(std::uint64_t seed, int rollouts = kDefaultRollouts);

    void begin_hand(int hand_number) override;
    Action decide(const PlayerView& view) override { return active_->decide(view); }
    std::string_view name() const override { return "Shifter"; }
    std::string hand_log_label() const override { return "Shift:" + std::string(active_->name()); }

private:
    void pick_profile();

    std::mt19937_64 rng_;
    std::array<std::unique_ptr<IBotStrategy>, 4> profiles_;
    IBotStrategy* active_ = nullptr;
};

enum class BotKind : std::uint8_t { Nit, Loose, Gto, WildGto, Shifter };

inline constexpr std::array<BotKind, 5> kAllBotKinds{BotKind::Nit, BotKind::Loose, BotKind::Gto, BotKind::WildGto,
                                                     BotKind::Shifter};

// `rollouts` is there for tests and tuning; the game uses the default.
std::unique_ptr<IBotStrategy> make_bot(BotKind kind, std::uint64_t seed, int rollouts = kDefaultRollouts);

}  // namespace poker
