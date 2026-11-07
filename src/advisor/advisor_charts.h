#pragma once

#include "engine/player_view.h"
#include "strategy/preflop_charts.h"

namespace poker {

// The advisor's preflop chart: the bots' chart_response (bots/gto_heuristic_engine.h)
// with the advisor's own tables where they exist. The bots never call it, and
// their charts are unchanged.
//
//   Push/fold    With an effective stack of at most 15 big blinds, first in (no raise
//                and no limper) or facing a lone short all-in (one raise, all in, no
//                caller, and a jam depth of at most 15 big blinds), push_fold_response
//                (strategy/push_fold.h) decides. First in its Raise means all in;
//                facing the jam it answers Call or Fold. The effective stack is
//                Hero's starting stack (stack plus chips put in) against the deepest
//                other player still in, or against the jam; it is rounded to whole
//                big blinds and read at 3 BB below that. The jam depth is what the
//                range model reads the jam at: the smaller of the jammer's street
//                total and the deepest starting stack among the other players dealt
//                in (Hero included) who had not folded when it was made.
//   Deep jam     Facing a lone all-in deeper than 15 big blinds with an effective
//                stack of at most 15: no chart (`neutral`), so the advisor's range
//                EV against the jammer's commit range decides alone. The push/fold
//                call ranges assume a jam from that stack, and the commit chart
//                (vs_four_bet_response) prices a call of the whole jam, but Hero's
//                price here depends on Hero's stack: a 10 BB Hero needs about 44%
//                against a 100 BB jam from the BTN, a 3 BB Hero about 31%.
//   Facing a     As the opener: vs_three_bet_ranges(Hero, 3-bettor). Anyone else (a
//   3-bet        cold caller squeezed, a player yet to act) uses the bots'
//                non-positional vs_three_bet_response, as does a 3-bet that commits.
//   Facing a     As the 3-bettor: vs_four_bet_ranges(Hero, 4-bettor), committing or
//   4-bet        not, since the table is for the usual 4-bet, which commits at 100 BB.
//                Anyone else, and every later raise, uses vs_four_bet_response.
//   Facing one   advisor_vs_open_ranges(Hero, opener), unless the call commits: the
//   raise        advisor's own answers to an open, wider than vs_open_response.
//   Otherwise    chart_response's rules: the opens (the BB raises its premium and
//                strong tiers over limps), and vs_four_bet_response for a call that
//                commits.
// Raises are counted as chart_response counts them (every raise record). Then, as
// there, a chart bluff raise (suited wheel aces) that cannot be made folds, and a
// fold at a price of at most kCheapCallEvenShare of an even share becomes a call.
struct AdvisorChart {
    PreflopResponse response = PreflopResponse::Fold;
    bool push_fold = false;  // From the push/fold tables: first in, Raise means all in
    // A deep jam into a short Hero (see "Deep jam" above): no chart applies, and the
    // advisor uses no prior. `response` is then the commit chart's answer (Call for
    // a hand vs_four_bet_response plays, else Fold), for reference only.
    bool neutral = false;
};

// Throws std::invalid_argument unless the view holds Hero's cards.
AdvisorChart advisor_chart(const PlayerView& view);
PreflopResponse advisor_chart_response(const PlayerView& view);

}  // namespace poker
