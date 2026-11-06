#include "strategy/push_fold.h"

#include <array>
#include <cstddef>

namespace poker {
namespace {

constexpr auto kJammers = static_cast<std::size_t>(detail::kPushFoldJammerCount);
constexpr auto kPositions = static_cast<std::size_t>(detail::kPushFoldPositionCount);
constexpr auto kStacks = static_cast<std::size_t>(detail::kPushFoldStackCount);

HandRange from_bits(const std::uint64_t (&words)[detail::kHandSetWords]) {
    HandRange range;
    for (const HandClass hand : hand_classes_by_strength()) {
        const auto index = static_cast<std::size_t>(hand.index());
        if ((words[index / 64] >> (index % 64)) & 1u) range.add(hand);
    }
    return range;
}

struct Tables {
    std::array<std::array<HandRange, kStacks>, kJammers> jam;
    std::array<std::array<std::array<HandRange, kStacks>, kPositions>, kJammers> call;

    Tables() {
        for (std::size_t j = 0; j < kJammers; ++j) {
            for (std::size_t s = 0; s < kStacks; ++s) {
                jam[j][s] = from_bits(detail::kPushFoldJam[j][s]);
                for (std::size_t c = 0; c < kPositions; ++c) call[j][c][s] = from_bits(detail::kPushFoldCall[j][c][s]);
            }
        }
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

bool in_stack_range(int stack_bb) { return stack_bb >= kPushFoldMinStackBb && stack_bb <= kPushFoldMaxStackBb; }

// Heads-up the button posts the small blind and plays the SB game.
Position table_position(Position position, int players) {
    return players == 2 && position == Position::BTN ? Position::SB : position;
}

std::size_t stack_slot(int stack_bb) { return static_cast<std::size_t>(stack_bb - kPushFoldMinStackBb); }

}  // namespace

std::optional<HandRange> push_fold_jam_range(Position jammer, int stack_bb, int players) {
    const Position table_jammer = table_position(jammer, players);
    if (!in_stack_range(stack_bb) || table_jammer == Position::BB) return std::nullopt;
    return tables().jam[static_cast<std::size_t>(table_jammer)][stack_slot(stack_bb)];
}

std::optional<HandRange> push_fold_call_range(Position caller, Position jammer, int stack_bb, int players) {
    const Position from = table_position(jammer, players);
    const Position to = table_position(caller, players);
    if (!in_stack_range(stack_bb) || from == Position::BB || to <= from) return std::nullopt;
    return tables().call[static_cast<std::size_t>(from)][static_cast<std::size_t>(to)][stack_slot(stack_bb)];
}

std::optional<PreflopResponse> push_fold_response(HandClass hand, Position hero, int stack_bb,
                                                  std::optional<Position> shover, int players) {
    if (!shover) {
        const auto range = push_fold_jam_range(hero, stack_bb, players);
        if (!range) return std::nullopt;
        return range->contains(hand) ? PreflopResponse::Raise : PreflopResponse::Fold;
    }
    const auto range = push_fold_call_range(hero, *shover, stack_bb, players);
    if (!range) return std::nullopt;
    return range->contains(hand) ? PreflopResponse::Call : PreflopResponse::Fold;
}

int push_fold_iterations() { return detail::kPushFoldIterations; }

}  // namespace poker
