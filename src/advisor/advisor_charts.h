#pragma once

#include "engine/player_view.h"
#include "strategy/preflop_charts.h"

namespace poker {

// The advisor's preflop chart: the bots' chart_response (bots/gto_heuristic_engine.h)
// with the advisor's own tables where they exist. The bots never call it, and
// their charts are unchanged.
//
//   Push/fold    With an effective stack of at most 15 big blinds, first in (no raise
//                and no limper) or facing a lone all-in (one raise, all in, and no
//                caller), push_fold_response (strategy/push_fold.h) decides. First in
//                its Raise means all in; facing the jam it answers Call or Fold. The
//                effective stack is Hero's starting stack (stack plus chips put in)
//                against the deepest other player still in, or against the jam; it
//                is rounded to whole big blinds and read at 3 BB below that.
//   Facing a     As the opener: vs_three_bet_ranges(Hero, 3-bettor). Anyone else (a
//   3-bet        cold caller squeezed, a player yet to act) uses the bots'
//                non-positional vs_three_bet_response, as does a 3-bet that commits.
//   Facing a     As the 3-bettor: vs_four_bet_ranges(Hero, 4-bettor), committing or
//   4-bet        not, since the table is for the usual 4-bet, which commits at 100 BB.
//                Anyone else, and every later raise, uses vs_four_bet_response.
//   Otherwise    chart_response's rules: the opens (the BB raises its premium and
//                strong tiers over limps), vs_open_response against one raise, and
//                vs_four_bet_response for a call that commits.
// Raises are counted as chart_response counts them (every raise record). Then, as
// there, a chart bluff raise (suited wheel aces) that cannot be made folds, and a
// fold at a price of at most kCheapCallEvenShare of an even share becomes a call.
struct AdvisorChart {
    PreflopResponse response = PreflopResponse::Fold;
    bool push_fold = false;  // From the push/fold tables: first in, Raise means all in
};

// Throws std::invalid_argument unless the view holds Hero's cards.
AdvisorChart advisor_chart(const PlayerView& view);
PreflopResponse advisor_chart_response(const PlayerView& view);

}  // namespace poker
