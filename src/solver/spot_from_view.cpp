#include "solver/spot_from_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

#include "range/combos.h"

namespace poker {
namespace {

struct HeadsUp {
    std::array<int, 2> players{};  // Table indices: [0] OOP, [1] IP
    int viewer = 0;                // 0 or 1: which of them is view.seat
};

bool contesting(SeatStatus status) { return status == SeatStatus::Active || status == SeatStatus::AllIn; }

// Postflop action starts left of the button, so a player's place in the order is
// its distance clockwise from the button, the button itself last.
int postflop_order(int index, int button) { return (index - button - 1 + 2 * kMaxSeats) % kMaxSeats; }

HeadsUp heads_up_of(const PlayerView& view) {
    if (view.street != Street::Flop && view.street != Street::Turn && view.street != Street::River) {
        throw std::invalid_argument("spot_from_view: postflop only");
    }
    if (view.seat < 0 || view.seat >= kMaxSeats) throw std::invalid_argument("spot_from_view: viewer out of range");
    if (view.button < 0 || view.button >= kMaxSeats) throw std::invalid_argument("spot_from_view: no button");
    if (!view.to_act || *view.to_act != view.seat) {
        throw std::invalid_argument("spot_from_view: the viewer must be the one to act");
    }
    std::array<int, 2> found{};
    int count = 0;
    for (int index = 0; index < kMaxSeats; ++index) {
        if (!contesting(view.seats[static_cast<std::size_t>(index)].status)) continue;
        if (count == 2) throw std::invalid_argument("spot_from_view: more than two players contest the pot");
        found[static_cast<std::size_t>(count++)] = index;
    }
    if (count != 2) throw std::invalid_argument("spot_from_view: two players must contest the pot");
    if (found[0] != view.seat && found[1] != view.seat) {
        throw std::invalid_argument("spot_from_view: the viewer is not in the pot");
    }
    if (postflop_order(found[0], view.button) > postflop_order(found[1], view.button)) std::swap(found[0], found[1]);
    HeadsUp result;
    result.players = found;
    result.viewer = found[0] == view.seat ? 0 : 1;
    return result;
}

}  // namespace

int solver_player_of(const PlayerView& view) { return heads_up_of(view).viewer; }

SolveSpot spot_from_view(const PlayerView& view, const WeightedRange& hero_range, const WeightedRange& villain_range) {
    const HeadsUp heads_up = heads_up_of(view);
    SolveSpot spot;
    spot.board = view.board;

    std::array<int, 2> bet{};    // Street bets as the table has them
    std::array<int, 2> reach{};  // Street bet plus stack: the most each can have in this street
    for (std::size_t p = 0; p < 2; ++p) {
        const PublicSeat& player = view.seats[static_cast<std::size_t>(heads_up.players[p])];
        bet[p] = player.street_bet;
        reach[p] = player.street_bet + player.stack;
    }
    // A street bet beyond what the other player can reach is never called: the
    // engine returns the excess, so it is neither pot nor street bet here.
    const int cap = std::min(reach[0], reach[1]);
    const int uncalled = std::max(0, std::max(bet[0], bet[1]) - cap);
    for (std::size_t p = 0; p < 2; ++p) spot.street_bet[p] = std::min(bet[p], cap);
    const int larger_bet = std::max(spot.street_bet[0], spot.street_bet[1]);
    spot.pot = view.total_pot - uncalled - spot.street_bet[0] - spot.street_bet[1];
    spot.effective_stack = std::max(0, cap - larger_bet);
    spot.to_act = heads_up.viewer;
    spot.last_raise = view.last_raise_size;
    spot.raises_made = static_cast<int>(std::count_if(view.actions.begin(), view.actions.end(), [&](const ActionRecord& a) {
        return a.street == view.street && a.type == ActionType::Raise;
    }));
    spot.min_bet = std::max(1, view.big_blind);

    const CardMask board = card_mask(std::span<const Card>(view.board));
    const auto hero = static_cast<std::size_t>(heads_up.viewer);
    spot.ranges[hero] = hero_range;
    spot.ranges[1 - hero] = villain_range;
    spot.ranges[0].remove_cards(board);
    spot.ranges[1].remove_cards(board);
    return spot;
}

}  // namespace poker
