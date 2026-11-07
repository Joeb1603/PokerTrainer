#include "solver/showdown.h"

#include <algorithm>
#include <stdexcept>

#include "range/combos.h"

namespace poker {
namespace {

template <typename T>
std::size_t capacity_bytes(const std::vector<T>& values) {
    return values.capacity() * sizeof(T);
}

std::array<Card, 5> five_cards(std::span<const Card> board) {
    if (board.size() != 5) throw std::invalid_argument("showdown: the board must have 5 cards");
    return {board[0], board[1], board[2], board[3], board[4]};
}

CardMask mask_of(std::span<const Card> board) {
    CardMask mask = 0;
    for (const Card card : board) mask |= card_mask(card);
    return mask;
}

bool shares_card(const std::array<std::uint8_t, 2>& a, const std::array<std::uint8_t, 2>& b) {
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

}  // namespace

MatchupHands::MatchupHands(std::span<const int> combos0, std::span<const int> combos1) {
    const std::array<std::span<const int>, 2> lists{combos0, combos1};
    for (std::size_t p = 0; p < 2; ++p) {
        const std::span<const int> list = lists[p];
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] < 0 || list[i] >= kComboCount || (i > 0 && list[i] <= list[i - 1])) {
                throw std::invalid_argument("matchup hands: combos must be valid and strictly ascending");
            }
        }
        combos_[p].assign(list.begin(), list.end());
        cards_[p].reserve(list.size());
        std::array<int, kDeckSize> per_card{};
        for (const int combo : list) {
            const std::array<Card, 2> cards = combo_cards(combo);
            cards_[p].push_back({cards[0].id, cards[1].id});
            ++per_card[cards[0].id];
            ++per_card[cards[1].id];
        }
        for (std::size_t c = 0; c < static_cast<std::size_t>(kDeckSize); ++c) starts_[p][c + 1] = starts_[p][c] + per_card[c];
        by_card_[p].resize(2 * list.size());
        std::array<int, kDeckSize + 1> fill = starts_[p];
        for (std::size_t h = 0; h < list.size(); ++h) {
            for (const std::uint8_t card : cards_[p][h]) {
                by_card_[p][static_cast<std::size_t>(fill[card]++)] = static_cast<int>(h);
            }
        }
    }
    for (std::size_t p = 0; p < 2; ++p) {
        const std::vector<int>& theirs = combos_[1 - p];
        same_[p].reserve(combos_[p].size());
        for (const int combo : combos_[p]) {
            const auto it = std::lower_bound(theirs.begin(), theirs.end(), combo);
            same_[p].push_back(it != theirs.end() && *it == combo ? static_cast<int>(it - theirs.begin()) : -1);
        }
    }
}

std::span<const int> MatchupHands::with_card(int player, int card_id) const {
    const auto p = static_cast<std::size_t>(player);
    const auto c = static_cast<std::size_t>(card_id);
    return {by_card_[p].data() + starts_[p][c], static_cast<std::size_t>(starts_[p][c + 1] - starts_[p][c])};
}

std::size_t MatchupHands::bytes() const {
    std::size_t total = 0;
    for (std::size_t p = 0; p < 2; ++p) {
        total += capacity_bytes(combos_[p]) + capacity_bytes(cards_[p]) + capacity_bytes(same_[p]) +
                 capacity_bytes(by_card_[p]);
    }
    return total;
}

std::size_t MatchupHands::bytes_for(int hands0, int hands1) {
    const auto n = static_cast<std::size_t>(hands0 + hands1);
    return n * (sizeof(int) + sizeof(std::array<std::uint8_t, 2>) + sizeof(int) + 2 * sizeof(int));
}

void fold_values(const MatchupHands& hands, int player, std::span<const double> opp_reach, double payoff,
                 std::span<double> out) {
    const int opponent = 1 - player;
    double total = 0.0;
    std::array<double, kDeckSize> per_card{};
    for (int o = 0; o < hands.size(opponent); ++o) {
        const double reach = opp_reach[static_cast<std::size_t>(o)];
        const std::array<std::uint8_t, 2>& cards = hands.cards(opponent, o);
        total += reach;
        per_card[cards[0]] += reach;
        per_card[cards[1]] += reach;
    }
    for (int h = 0; h < hands.size(player); ++h) {
        const std::array<std::uint8_t, 2>& cards = hands.cards(player, h);
        const int same = hands.same_combo(player, h);
        const double same_reach = same >= 0 ? opp_reach[static_cast<std::size_t>(same)] : 0.0;
        out[static_cast<std::size_t>(h)] = payoff * (total - per_card[cards[0]] - per_card[cards[1]] + same_reach);
    }
}

ShowdownTable::ShowdownTable(const MatchupHands& hands, std::span<const Card> board) {
    const std::array<Card, 5> five = five_cards(board);
    const CardMask dead = mask_of(board);
    for (int p = 0; p < 2; ++p) {
        const auto index = static_cast<std::size_t>(p);
        std::vector<std::pair<HandRank, int>> ranked;
        ranked.reserve(static_cast<std::size_t>(hands.size(p)));
        for (int h = 0; h < hands.size(p); ++h) {
            const std::array<std::uint8_t, 2>& cards = hands.cards(p, h);
            if ((dead & ((CardMask{1} << cards[0]) | (CardMask{1} << cards[1]))) != 0) continue;
            ranked.emplace_back(evaluate7({Card(cards[0]), Card(cards[1])}, five), h);
        }
        // Weakest (highest rank number) first; ties by hand index for determinism.
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        // Reserved at the full list size, so the capacity is the same on every board.
        order_[index].reserve(static_cast<std::size_t>(hands.size(p)));
        rank_[index].reserve(static_cast<std::size_t>(hands.size(p)));
        for (const auto& [rank, hand] : ranked) {
            order_[index].push_back(hand);
            rank_[index].push_back(rank);
        }
    }
}

void ShowdownTable::values(const MatchupHands& hands, int player, std::span<const double> opp_reach, double win,
                           double tie, double lose, std::span<double> out) const {
    const auto me = static_cast<std::size_t>(player);
    const auto them = 1 - me;
    const int opponent = 1 - player;
    const std::vector<int>& mine = order_[me];
    const std::vector<HandRank>& my_rank = rank_[me];
    const std::vector<int>& theirs = order_[them];
    const std::vector<HandRank>& their_rank = rank_[them];
    std::fill(out.begin(), out.end(), 0.0);

    // Unblocked opponent reach, in total and per card.
    double total = 0.0;
    std::array<double, kDeckSize> per_card{};
    for (const int o : theirs) {
        const double reach = opp_reach[static_cast<std::size_t>(o)];
        const std::array<std::uint8_t, 2>& cards = hands.cards(opponent, o);
        total += reach;
        per_card[cards[0]] += reach;
        per_card[cards[1]] += reach;
    }

    // out = win * beaten + lose * beats_me + tie * (compatible - beaten - beats_me),
    // written as (win - tie) * beaten + (lose - tie) * beats_me + tie * compatible.
    const double win_weight = win - tie;
    const double lose_weight = lose - tie;

    double below = 0.0;
    std::array<double, kDeckSize> below_card{};
    std::size_t j = 0;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        while (j < theirs.size() && their_rank[j] > my_rank[i]) {
            const double reach = opp_reach[static_cast<std::size_t>(theirs[j])];
            const std::array<std::uint8_t, 2>& cards = hands.cards(opponent, theirs[j]);
            below += reach;
            below_card[cards[0]] += reach;
            below_card[cards[1]] += reach;
            ++j;
        }
        const std::array<std::uint8_t, 2>& cards = hands.cards(player, mine[i]);
        out[static_cast<std::size_t>(mine[i])] = win_weight * (below - below_card[cards[0]] - below_card[cards[1]]);
    }

    double above = 0.0;
    std::array<double, kDeckSize> above_card{};
    std::size_t k = theirs.size();
    for (std::size_t i = mine.size(); i-- > 0;) {
        while (k > 0 && their_rank[k - 1] < my_rank[i]) {
            --k;
            const double reach = opp_reach[static_cast<std::size_t>(theirs[k])];
            const std::array<std::uint8_t, 2>& cards = hands.cards(opponent, theirs[k]);
            above += reach;
            above_card[cards[0]] += reach;
            above_card[cards[1]] += reach;
        }
        const int h = mine[i];
        const std::array<std::uint8_t, 2>& cards = hands.cards(player, h);
        const int same = hands.same_combo(player, h);
        const double same_reach = same >= 0 ? opp_reach[static_cast<std::size_t>(same)] : 0.0;
        const double compatible = total - per_card[cards[0]] - per_card[cards[1]] + same_reach;
        out[static_cast<std::size_t>(h)] +=
            lose_weight * (above - above_card[cards[0]] - above_card[cards[1]]) + tie * compatible;
    }
}

std::size_t ShowdownTable::bytes() const {
    return capacity_bytes(order_[0]) + capacity_bytes(order_[1]) + capacity_bytes(rank_[0]) + capacity_bytes(rank_[1]);
}

std::size_t ShowdownTable::bytes_for(int hands0, int hands1) {
    return static_cast<std::size_t>(hands0 + hands1) * (sizeof(int) + sizeof(HandRank));
}

CheckDownTable::CheckDownTable(const MatchupHands& hands, std::span<const Card> turn_board) {
    if (turn_board.size() != 4) throw std::invalid_argument("check-down table: the board must have 4 cards");
    const CardMask dead = mask_of(turn_board);
    const auto n0 = static_cast<std::size_t>(hands.size(0));
    const auto n1 = static_cast<std::size_t>(hands.size(1));
    // Built [h0][h1] for both players (the inner loop runs over h1), then player 0's
    // transposed so each player's rows are the opponent's hands.
    std::vector<std::uint8_t> by_h0(n0 * n1, 0);
    std::vector<std::uint8_t> theirs(n0 * n1, 0);
    std::vector<int> rank0(n0);
    std::vector<int> rank1(n1);
    std::vector<std::uint8_t> live1(n1);
    std::array<Card, 5> five{turn_board[0], turn_board[1], turn_board[2], turn_board[3], Card(0)};
    const auto holds = [](const std::array<std::uint8_t, 2>& cards, CardMask mask) {
        return (mask & ((CardMask{1} << cards[0]) | (CardMask{1} << cards[1]))) != 0;
    };
    for (int river = 0; river < kDeckSize; ++river) {
        const CardMask river_mask = CardMask{1} << river;
        if ((dead & river_mask) != 0) continue;
        five[4] = Card(static_cast<std::uint8_t>(river));
        const CardMask blocked = dead | river_mask;
        for (std::size_t h = 0; h < n0; ++h) {
            const std::array<std::uint8_t, 2>& cards = hands.cards(0, static_cast<int>(h));
            rank0[h] = holds(cards, blocked) ? -1 : static_cast<int>(evaluate7({Card(cards[0]), Card(cards[1])}, five));
        }
        for (std::size_t o = 0; o < n1; ++o) {
            const std::array<std::uint8_t, 2>& cards = hands.cards(1, static_cast<int>(o));
            const bool live = !holds(cards, blocked);
            live1[o] = live ? 1 : 0;
            rank1[o] = live ? static_cast<int>(evaluate7({Card(cards[0]), Card(cards[1])}, five)) : 0;
        }
        for (std::size_t h = 0; h < n0; ++h) {
            const int mine = rank0[h];
            if (mine < 0) continue;
            std::uint8_t* row0 = by_h0.data() + h * n1;
            std::uint8_t* row1 = theirs.data() + h * n1;
            // Lower rank numbers are stronger.
            for (std::size_t o = 0; o < n1; ++o) {
                const int other = rank1[o];
                const int live = live1[o];
                row0[o] = static_cast<std::uint8_t>(row0[o] + live * ((mine < other ? 2 : 0) + (mine == other ? 1 : 0)));
                row1[o] = static_cast<std::uint8_t>(row1[o] + live * ((mine > other ? 2 : 0) + (mine == other ? 1 : 0)));
            }
        }
    }
    // Pairs sharing a card were counted as if they could be dealt together.
    for (std::size_t h = 0; h < n0; ++h) {
        for (const std::uint8_t card : hands.cards(0, static_cast<int>(h))) {
            for (const int o : hands.with_card(1, card)) {
                by_h0[h * n1 + static_cast<std::size_t>(o)] = 0;
                theirs[h * n1 + static_cast<std::size_t>(o)] = 0;
            }
        }
    }
    counts_[0].assign(n0 * n1, 0);
    for (std::size_t h = 0; h < n0; ++h) {
        for (std::size_t o = 0; o < n1; ++o) counts_[0][o * n0 + h] = by_h0[h * n1 + o];
    }
    counts_[1] = std::move(theirs);
}

void CheckDownTable::values(const MatchupHands& hands, int player, std::span<const double> opp_reach, double pot,
                            double invested, std::span<double> out) const {
    const auto n = static_cast<std::size_t>(hands.size(player));
    const auto rows = static_cast<std::size_t>(hands.size(1 - player));
    fold_values(hands, player, opp_reach, -invested, out);
    const double unit = pot / 88.0;
    const std::uint8_t* counts = counts_[static_cast<std::size_t>(player)].data();
    for (std::size_t o = 0; o < rows; ++o) {
        const double reach = opp_reach[o];
        if (reach == 0.0) continue;
        const double weight = unit * reach;
        const std::uint8_t* row = counts + o * n;
        for (std::size_t h = 0; h < n; ++h) out[h] += weight * static_cast<double>(row[h]);
    }
}

std::size_t CheckDownTable::bytes() const { return capacity_bytes(counts_[0]) + capacity_bytes(counts_[1]); }

std::size_t CheckDownTable::bytes_for(int hands0, int hands1) {
    return 2 * static_cast<std::size_t>(hands0) * static_cast<std::size_t>(hands1);
}

void fold_values_reference(const MatchupHands& hands, int player, std::span<const double> opp_reach, double payoff,
                           std::span<double> out) {
    const int opponent = 1 - player;
    for (int h = 0; h < hands.size(player); ++h) {
        double sum = 0.0;
        for (int o = 0; o < hands.size(opponent); ++o) {
            if (!shares_card(hands.cards(player, h), hands.cards(opponent, o))) sum += opp_reach[static_cast<std::size_t>(o)];
        }
        out[static_cast<std::size_t>(h)] = payoff * sum;
    }
}

void showdown_values_reference(const MatchupHands& hands, std::span<const Card> board, int player,
                               std::span<const double> opp_reach, double win, double tie, double lose,
                               std::span<double> out) {
    const std::array<Card, 5> five = five_cards(board);
    const CardMask dead = mask_of(board);
    const int opponent = 1 - player;
    const auto blocked = [&](const std::array<std::uint8_t, 2>& cards) {
        return (dead & ((CardMask{1} << cards[0]) | (CardMask{1} << cards[1]))) != 0;
    };
    for (int h = 0; h < hands.size(player); ++h) {
        const std::array<std::uint8_t, 2>& mine = hands.cards(player, h);
        double sum = 0.0;
        if (!blocked(mine)) {
            const HandRank my_rank = evaluate7({Card(mine[0]), Card(mine[1])}, five);
            for (int o = 0; o < hands.size(opponent); ++o) {
                const std::array<std::uint8_t, 2>& theirs = hands.cards(opponent, o);
                if (blocked(theirs) || shares_card(mine, theirs)) continue;
                const HandRank their_rank = evaluate7({Card(theirs[0]), Card(theirs[1])}, five);
                const double payoff = my_rank < their_rank ? win : my_rank > their_rank ? lose : tie;
                sum += opp_reach[static_cast<std::size_t>(o)] * payoff;
            }
        }
        out[static_cast<std::size_t>(h)] = sum;
    }
}

}  // namespace poker
