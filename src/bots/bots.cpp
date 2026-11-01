#include "bots/bots.h"

#include <stdexcept>

namespace poker {
namespace {

constexpr std::uint64_t kWildRollStream = 0x9E3779B97F4A7C15ULL;

// One in four overrides of a fold is a bluff raise, the rest are light calls.
constexpr double kWildBluffRaiseShare = 0.25;

Action overbet(const PlayerView& view) {
    const ActionType type = view.legal.allows(ActionType::Bet) ? ActionType::Bet : ActionType::Raise;
    return legalize(view, {type, raise_to_for_pot_fraction(view, kWildOverbetFraction)});
}

bool is_aggressive(const Action& action) {
    return action.type == ActionType::Bet || action.type == ActionType::Raise || action.type == ActionType::AllIn;
}

bool can_aggress(const PlayerView& view) {
    return view.legal.allows(ActionType::Bet) || view.legal.allows(ActionType::Raise);
}

Action uncautious_action(const PlayerView& view, const Action& gto, std::mt19937_64& rng) {
    if (gto.type == ActionType::Fold && view.legal.allows(ActionType::Call)) {
        std::uniform_real_distribution<double> roll(0.0, 1.0);
        const bool bluff_raise = can_aggress(view) && roll(rng) < kWildBluffRaiseShare;
        return bluff_raise ? overbet(view) : Action{ActionType::Call, 0};
    }

    if (!can_aggress(view)) return gto;  // Nothing bolder than the GTO call or check exists

    const Action big = overbet(view);
    if (is_aggressive(gto) && gto.amount >= big.amount) {
        return view.legal.allows(ActionType::AllIn) ? Action{ActionType::AllIn, 0} : gto;
    }
    return big;
}

}  // namespace

WildGtoBot::WildGtoBot(std::uint64_t seed, int rollouts) : gto_(seed, rollouts), rng_(seed ^ kWildRollStream) {}

Action WildGtoBot::decide(const PlayerView& view) {
    const Action gto = gto_.decide(view);
    std::uniform_real_distribution<double> roll(0.0, 1.0);
    last_roll_overrode_ = roll(rng_) < kWildOverrideProbability;
    if (!last_roll_overrode_ || !view.own_hole || view.legal.mask == 0) return gto;
    return uncautious_action(view, gto, rng_);
}

ShifterBot::ShifterBot(std::uint64_t seed, int rollouts) : rng_(seed) {
    // Derived, so the sub-bots' streams never overlap the profile picks.
    profiles_[0] = std::make_unique<NitBot>(rng_(), rollouts);
    profiles_[1] = std::make_unique<LooseBot>(rng_(), rollouts);
    profiles_[2] = std::make_unique<GtoBot>(rng_(), rollouts);
    profiles_[3] = std::make_unique<WildGtoBot>(rng_(), rollouts);
    pick_profile();  // So a decide() before any begin_hand still has a profile
}

void ShifterBot::pick_profile() {
    std::uniform_int_distribution<std::size_t> pick(0, profiles_.size() - 1);
    active_ = profiles_[pick(rng_)].get();
}

void ShifterBot::begin_hand(int hand_number) {
    pick_profile();
    active_->begin_hand(hand_number);
}

std::unique_ptr<IBotStrategy> make_bot(BotKind kind, std::uint64_t seed, int rollouts) {
    switch (kind) {
        case BotKind::Nit: return std::make_unique<NitBot>(seed, rollouts);
        case BotKind::Loose: return std::make_unique<LooseBot>(seed, rollouts);
        case BotKind::Gto: return std::make_unique<GtoBot>(seed, rollouts);
        case BotKind::WildGto: return std::make_unique<WildGtoBot>(seed, rollouts);
        case BotKind::Shifter: return std::make_unique<ShifterBot>(seed, rollouts);
    }
    throw std::invalid_argument("make_bot: unknown kind");
}

}  // namespace poker
