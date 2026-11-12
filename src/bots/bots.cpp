#include "bots/bots.h"

#include <stdexcept>
#include <utility>

#include "core/random.h"

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

TiltBot::TiltBot(std::uint64_t seed, int rollouts) {
    std::mt19937_64 derive(seed);
    calm_ = std::make_unique<ProfileBot>(tag_profile(rollouts), derive());
    tilted_ = std::make_unique<ProfileBot>(maniac_profile(rollouts), derive());
}

void TiltBot::reset() {
    baseline_.reset();
    last_hand_ = 0;
    tilt_until_hand_ = 0;
}

void TiltBot::begin_hand(int hand_number) {
    if (hand_number <= last_begun_) reset();  // Hand numbers restarted: a new session
    last_begun_ = hand_number;
    // Set here too, so the label is right for a hand it never acts in.
    tilted_now_ = hand_number <= tilt_until_hand_;
    calm_->begin_hand(hand_number);
    tilted_->begin_hand(hand_number);
}

Action TiltBot::decide(const PlayerView& view) {
    if (view.hand_number != last_hand_) {
        if (view.hand_number < last_hand_) reset();
        const PublicSeat& me = view.seats[static_cast<std::size_t>(view.seat)];
        const int start = me.stack + me.total_bet;
        if (baseline_ && *baseline_ - start >= kTiltLossBigBlinds * view.big_blind) {
            tilt_until_hand_ = view.hand_number + kTiltHands - 1;
        }
        tilted_now_ = view.hand_number <= tilt_until_hand_;
        baseline_ = start;
        last_hand_ = view.hand_number;
    }
    return tilted_now_ ? tilted_->decide(view) : calm_->decide(view);
}

std::unique_ptr<IBotStrategy> make_bot(BotKind kind, std::uint64_t seed, int rollouts) {
    switch (kind) {
        case BotKind::Nit: return std::make_unique<NitBot>(seed, rollouts);
        case BotKind::Loose: return std::make_unique<LooseBot>(seed, rollouts);
        case BotKind::Gto: return std::make_unique<GtoBot>(seed, rollouts);
        case BotKind::WildGto: return std::make_unique<WildGtoBot>(seed, rollouts);
        case BotKind::Shifter: return std::make_unique<ShifterBot>(seed, rollouts);
        case BotKind::Tag: return std::make_unique<ProfileBot>(tag_profile(rollouts), seed);
        case BotKind::Lag: return std::make_unique<ProfileBot>(lag_profile(rollouts), seed);
        case BotKind::Station: return std::make_unique<ProfileBot>(station_profile(rollouts), seed);
        case BotKind::Maniac: return std::make_unique<ProfileBot>(maniac_profile(rollouts), seed);
        case BotKind::Rock: return std::make_unique<ProfileBot>(rock_profile(rollouts), seed);
        case BotKind::Trapper: return std::make_unique<ProfileBot>(trapper_profile(rollouts), seed);
        case BotKind::Bully: return std::make_unique<ProfileBot>(bully_profile(rollouts), seed);
        case BotKind::Tilter: return std::make_unique<TiltBot>(seed, rollouts);
    }
    throw std::invalid_argument("make_bot: unknown kind");
}

std::string_view bot_kind_name(BotKind kind) {
    switch (kind) {
        case BotKind::Nit: return "Nit";
        case BotKind::Loose: return "Loose";
        case BotKind::Gto: return "GTO";
        case BotKind::WildGto: return "WildGTO";
        case BotKind::Shifter: return "Shifter";
        case BotKind::Tag: return "TAG";
        case BotKind::Lag: return "LAG";
        case BotKind::Station: return "Station";
        case BotKind::Maniac: return "Maniac";
        case BotKind::Rock: return "Rock";
        case BotKind::Trapper: return "Trapper";
        case BotKind::Bully: return "Bully";
        case BotKind::Tilter: return "Tilter";
    }
    throw std::invalid_argument("bot_kind_name: unknown kind");
}

std::array<BotKind, 5> draw_random_lineup(std::mt19937_64& rng) {
    // A partial Fisher-Yates: the first five places of a shuffled pool.
    std::array<BotKind, kRandomPoolKinds.size()> pool = kRandomPoolKinds;
    std::array<BotKind, 5> lineup{};
    for (std::size_t i = 0; i < lineup.size(); ++i) {
        const std::size_t j = i + static_cast<std::size_t>(uniform_below(rng, pool.size() - i));
        std::swap(pool[i], pool[j]);
        lineup[i] = pool[i];
    }
    return lineup;
}

}  // namespace poker
