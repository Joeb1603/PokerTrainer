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

inline constexpr int kTiltLossBigBlinds = 30;
inline constexpr int kTiltHands = 8;  // The hand the loss is seen in, plus seven more

// Plays TAG until it has lost kTiltLossBigBlinds or more since the last hand it
// played, then plays Maniac for kTiltHands hands. It reads the loss from its own stack
// at its first decision of a hand: stack plus total_bet is where the hand started,
// since blinds count in total_bet. A hand number lower than the last one means a new
// session, which resets it. name() stays "Tilter"; hand_log_label() says whether it is
// on tilt, for the hand log only.
class TiltBot final : public IBotStrategy {
public:
    explicit TiltBot(std::uint64_t seed, int rollouts = kDefaultRollouts);

    void begin_hand(int hand_number) override;
    Action decide(const PlayerView& view) override;
    std::string_view name() const override { return "Tilter"; }
    std::string hand_log_label() const override { return tilted_now_ ? "Tilt:On" : "Tilt:Off"; }

    bool tilted() const { return tilted_now_; }

private:
    void reset();

    std::unique_ptr<IBotStrategy> calm_;
    std::unique_ptr<IBotStrategy> tilted_;
    std::optional<int> baseline_;  // Chips at the start of the last hand it played
    int last_hand_ = 0;            // Last hand it made a decision in
    int last_begun_ = 0;           // Last hand begin_hand announced
    int tilt_until_hand_ = 0;
    bool tilted_now_ = false;
};

// Nit, Loose, Gto, WildGto and Shifter are the classic lineup; the rest exist for the
// random-personalities pool. New kinds go at the end, so the values never move.
enum class BotKind : std::uint8_t {
    Nit,
    Loose,
    Gto,
    WildGto,
    Shifter,
    Tag,
    Lag,
    Station,
    Maniac,
    Rock,
    Trapper,
    Bully,
    Tilter
};

// The classic fixed lineup, in table order. Kept as it is: the default table and the
// tests that pin it depend on this order.
inline constexpr std::array<BotKind, 5> kAllBotKinds{BotKind::Nit, BotKind::Loose, BotKind::Gto, BotKind::WildGto,
                                                     BotKind::Shifter};

// The kinds added for random personalities.
inline constexpr std::array<BotKind, 8> kExtraBotKinds{BotKind::Tag,  BotKind::Lag,     BotKind::Station,
                                                       BotKind::Maniac, BotKind::Rock, BotKind::Trapper,
                                                       BotKind::Bully,  BotKind::Tilter};

// Everything a random lineup draws from.
inline constexpr std::array<BotKind, 13> kRandomPoolKinds{
    BotKind::Nit,    BotKind::Loose, BotKind::Gto,  BotKind::WildGto, BotKind::Shifter, BotKind::Tag,   BotKind::Lag,
    BotKind::Station, BotKind::Maniac, BotKind::Rock, BotKind::Trapper, BotKind::Bully, BotKind::Tilter};

// `rollouts` is there for tests and tuning; the game uses the default.
std::unique_ptr<IBotStrategy> make_bot(BotKind kind, std::uint64_t seed, int rollouts = kDefaultRollouts);

// The style a kind plays, as make_bot(kind)->name() gives it: "Nit", "WildGTO",
// "Shifter", "Tilter" and so on.
std::string_view bot_kind_name(BotKind kind);

// Five distinct kinds from kRandomPoolKinds in random order, which is the order of the
// five bot places at the table. Uses raw generator bits, so a seed draws the same
// lineup on every standard library.
std::array<BotKind, 5> draw_random_lineup(std::mt19937_64& rng);

}  // namespace poker
