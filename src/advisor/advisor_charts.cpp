#include "advisor/advisor_charts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "bots/gto_heuristic_engine.h"
#include "strategy/push_fold.h"

namespace poker {
namespace {

std::size_t at(int index) { return static_cast<std::size_t>(index); }

// The preflop raise records, in order: the raiser's index and whether it went all in.
struct PreflopRaise {
    int player = 0;
    bool all_in = false;
};

std::vector<PreflopRaise> preflop_raises(const PlayerView& view) {
    std::vector<PreflopRaise> out;
    for (const ActionRecord& a : view.actions) {
        if (a.street == Street::Preflop && a.type == ActionType::Raise) out.push_back({a.seat, a.all_in});
    }
    return out;
}

int preflop_calls(const PlayerView& view) {
    return static_cast<int>(std::count_if(view.actions.begin(), view.actions.end(), [](const ActionRecord& a) {
        return a.street == Street::Preflop && a.type == ActionType::Call;
    }));
}

Position position_of(const PlayerView& view, int index) {
    return view.seats[at(index)].position.value_or(Position::BTN);
}

int starting_stack(const PublicSeat& s) { return s.stack + s.total_bet; }

// The bots' committing-call test (call_is_committing in gto_heuristic_engine.cpp):
// over 10 BB, and either at least 50 BB or at least a third of what is at stake.
bool call_commits(const PlayerView& view) {
    constexpr int kPriceBigBlinds = 10;
    constexpr int kAlwaysBigBlinds = 50;
    const int call = view.legal.call_amount;
    if (call <= view.big_blind * kPriceBigBlinds) return false;
    if (call >= view.big_blind * kAlwaysBigBlinds) return true;
    const PublicSeat& me = view.seats[at(view.seat)];
    int deepest = 0;
    for (int i = 0; i < kMaxSeats; ++i) {
        const PublicSeat& s = view.seats[at(i)];
        if (i == view.seat || (s.status != SeatStatus::Active && s.status != SeatStatus::AllIn)) continue;
        deepest = std::max(deepest, s.street_bet + s.stack);
    }
    return call * 3 >= std::min(me.stack, deepest - me.street_bet);
}

// The push/fold table's answer, where one applies.
std::optional<PreflopResponse> push_fold_answer(const PlayerView& view, HandClass hand, int players,
                                                const std::vector<PreflopRaise>& raises, int calls) {
    if (calls > 0 || raises.size() > 1 || view.big_blind <= 0) return std::nullopt;
    const Position position = view.position.value_or(Position::BTN);
    const int mine = starting_stack(view.seats[at(view.seat)]);
    std::optional<Position> shover;
    int theirs = 0;
    if (raises.empty()) {
        for (int i = 0; i < kMaxSeats; ++i) {
            const PublicSeat& s = view.seats[at(i)];
            if (i != view.seat && s.position && s.status == SeatStatus::Active) theirs = std::max(theirs, starting_stack(s));
        }
    } else {
        if (!raises.front().all_in) return std::nullopt;
        shover = position_of(view, raises.front().player);
        theirs = starting_stack(view.seats[at(raises.front().player)]);
    }
    const double effective_bb = static_cast<double>(std::min(mine, theirs)) / view.big_blind;
    if (!(effective_bb <= kPushFoldMaxStackBb)) return std::nullopt;
    const int stack_bb =
        std::clamp(static_cast<int>(std::lround(effective_bb)), kPushFoldMinStackBb, kPushFoldMaxStackBb);
    return push_fold_response(hand, position, stack_bb, shover, players);
}

PreflopResponse respond_to(const VsRaiseRanges& ranges, HandClass hand) {
    if (ranges.raise.contains(hand)) return PreflopResponse::Raise;
    return ranges.call.contains(hand) ? PreflopResponse::Call : PreflopResponse::Fold;
}

bool chart_bluff_raise(HandClass hand) { return hand.suited && hand.high == Rank::Ace && hand.low <= Rank::Five; }

}  // namespace

AdvisorChart advisor_chart(const PlayerView& view) {
    if (!view.own_hole) throw std::invalid_argument("advisor_chart: the view holds no cards");
    const HandClass hand = HandClass::from_cards((*view.own_hole)[0], (*view.own_hole)[1]);
    const Position position = view.position.value_or(Position::BTN);
    int players = 0;
    for (const PublicSeat& s : view.seats) players += s.position ? 1 : 0;
    const std::vector<PreflopRaise> raises = preflop_raises(view);
    const int calls = preflop_calls(view);
    const int count = static_cast<int>(raises.size());

    AdvisorChart out;
    if (const auto table = push_fold_answer(view, hand, players, raises, calls)) {
        out.response = *table;
        out.push_fold = true;
    } else if (count == 0) {
        // Limped to the big blind: raise for value, check the rest for free.
        const bool raises_first_in = position == Position::BB ? preflop_tier(hand) <= PreflopTier::Strong
                                                              : should_open(hand, position, players);
        out.response = raises_first_in ? PreflopResponse::Raise : PreflopResponse::Fold;
    } else if (count == 3 && raises[1].player == view.seat) {
        out.response = respond_to(vs_four_bet_ranges(position, position_of(view, raises[2].player), players), hand);
    } else if (count >= 3 || call_commits(view)) {
        out.response = vs_four_bet_response(hand);
    } else if (count == 1) {
        out.response = vs_open_response(hand, position, position_of(view, raises[0].player), players);
    } else if (raises[0].player == view.seat) {
        out.response = respond_to(vs_three_bet_ranges(position, position_of(view, raises[1].player), players), hand);
    } else {
        out.response = vs_three_bet_response(hand);
    }

    const bool can_raise = view.legal.allows(ActionType::Bet) || view.legal.allows(ActionType::Raise);
    if (out.response == PreflopResponse::Raise && !can_raise && chart_bluff_raise(hand)) {
        out.response = PreflopResponse::Fold;
    }
    // The ranges assume a normal price. At a tiny one no range is worth folding.
    if (out.response == PreflopResponse::Fold) {
        const PotMath math = pot_math(view);
        const double even_share = 1.0 / (view.opponents_in_hand() + 1);
        if (math.to_call > 0 && math.pot_odds <= kCheapCallEvenShare * even_share) out.response = PreflopResponse::Call;
    }
    return out;
}

PreflopResponse advisor_chart_response(const PlayerView& view) { return advisor_chart(view).response; }

}  // namespace poker
