# River benchmark spots

`river_spots.json` holds 36 heads-up river decisions. `tools/advisor_bench` uses
them to score the Training Advisor against the DCFR river solver.
`tests/bench_test.cpp` checks that every spot parses and replays legally.

The ranges are **assumed, not solved**. They are judgement calls about who reaches
each spot with what, and the benchmark measures the advisor against an equilibrium
*given those ranges*. A different range author would get different numbers.

## Running

From the repository root, after a Release build:

```
advisor_bench [--spots PATH] [--iterations N] [--min-iterations N] [--target PCT]
              [--combos N] [--advisor-rollouts N] [--seed S] [--timing] [--only ID]
              [--csv PATH]
```

Defaults: `--iterations 3000`, `--min-iterations 200`, `--target 0.3` (% of pot),
`--combos 80`, `--advisor-rollouts 5000`, `--seed 0`.

## File format

The top level holds `stack` (2000), `small_blind` (10), `big_blind` (20) and
`spots`. Each spot has these fields:

| Field | Meaning |
|---|---|
| `id` | `<pot type>-<board family>-<line>`, unique |
| `description` | The line in words and the reasoning behind its ranges |
| `pot_type` | `btn_bb`, `co_btn` or `sb_btn_3bet` |
| `family` | `dry`, `wet`, `paired` or `monotone` |
| `line` | `xx` (no flop bet), `bx` (flop bet and call, turn checked) or `bb` (flop and turn bet and call) |
| `board` | Five cards, e.g. `"Kd 7c 2h 4s 9d"` |
| `pre`, `flop`, `turn`, `river` | Scripted actions, described below. `river` stops at the hero's decision |
| `hero` | `oop` or `ip`: the player whose river decision is scored |
| `oop_range`, `ip_range` | Each player's range at the hero's decision, after every scripted action |
| `pot`, `stacks` | Expected chips in the middle and `[oop, ip]` stacks behind at the hero's decision, which the test checks against the replay |

The table always has six players with the button on index 0, so BTN is 0, SB 1,
BB 2, UTG 3, HJ 4 and CO 5. Everyone starts with `stack` chips. The two players
who never fold preflop contest the pot.

### Action syntax

Actions are separated by spaces. Each one is `POS:ACT`, where `POS` is `UTG`,
`HJ`, `CO`, `BTN`, `SB` or `BB`:

| `ACT` | Action |
|---|---|
| `F` | Fold |
| `X` | Check |
| `C` | Call |
| `A` | All-in |
| `B<n>` / `R<n>` | Bet or raise to a street total of `n` chips: `R50` raises to 50 |
| `B<p>%` / `R<p>%` | Bet or raise by `p`% of the pot after calling, sized like the advisor's buttons (`raise_to_for_pot_fraction`): `B33%` |

A replay fails if an action comes out of turn, is illegal, or a street ends before
or after its script does.

### Range syntax

Ranges use `HandRange` syntax (`"66+, A2s+, KTs, JJ-88, AQo-ATo"`). Each
comma-separated token may end in `:weight` (default 1). A later token overrides
an earlier one for the combos they share. Two token forms reach single combos:

- `<classes>@<suit>` keeps only the combos of those classes in that suit. For
  suited classes both cards must be that suit, and for offsuit classes and pairs
  at least one card. `A2s+@h` is the heart flushes with an ace, and `AJo@s` is
  AJo with a spade.
- Four characters naming two cards, such as `AhKh`, is that one combo.

Write class tokens before suit-filtered ones, so the filter isn't overwritten:
`"KQs:0.3, KQs@h"` puts KhQh at 1 and the other KQs combos at 0.3.

Board cards are removed when a spot is solved. Combos the board makes impossible,
such as `QJs@h` with the Qh on the board, are simply dropped.

## The 36 spots

There are 3 pot types × 4 board families × 3 lines:

| Pot type | Preflop | OOP | IP | Pot on the flop |
|---|---|---|---|---|
| `btn_bb` | BTN opens to 2.5bb, SB folds, BB calls | BB | BTN | 110 |
| `co_btn` | CO opens to 2.5bb, BTN calls, blinds fold | CO | BTN | 130 |
| `sb_btn_3bet` | BTN opens to 2.5bb, SB 3-bets to 9bb, BB folds, BTN calls | SB | BTN | 380 |

The preflop aggressor makes every bet before the river: BTN in `btn_bb`, and the
out-of-position raiser in the other two. Flop bets are 33% of the pot. Turn bets
are 75%, except 50% in the 3-bet pot so a river bet still leaves chips behind.

- `xx`: the flop is checked through. On dry and paired boards the turn is checked
  through too. On wet and monotone boards OOP checks the turn, IP bets 50% and OOP
  calls.
- `bx`: the flop is bet and called, and the turn is checked through.
- `bb`: the flop and turn are bet and called.

The hero's role rotates, so each pot type and each board family has four spots of
each kind:

- OOP and first to act
- IP facing a 33% bet
- IP facing a 75% bet

A river bet always comes from OOP. When OOP was the preflop caller (BB in
`btn_bb`), that bet is a lead.

The boards are rainbow unless a flush is part of the family:

| Family | `btn_bb` | `co_btn` | `sb_btn_3bet` |
|---|---|---|---|
| Dry | Kd 7c 2h 4s 9d | As 8d 3c 5h Qs | Kc 8h 3d 2s 6c |
| Wet | Jh Ts 8h 4c 2s (the draws miss) | 9d 8d 6c 2s Ad (the flush arrives) | Th 9h 7c 6s 2c (the turn makes 8x a straight) |
| Paired | 7c 7d 4s Jh 2c | Qs Qd 6c 3h 9s | 8s 8c 3d Kc 2h |
| Monotone | Qh 8h 3h 5c Kd | 9s 6s 2s Kh Jd | Kd 9d 5d 2c 7s |

## How the ranges were built

- **Preflop starting ranges** come from the project's charts in
  `src/strategy/preflop_charts.cpp`:
  - `btn_bb`: the BTN `rfi_range`, and the call part of the BB's `vs_open_ranges`
    against BTN.
  - `co_btn`: the CO `rfi_range`, and BTN's call against a late open
    (`in_position_vs_late`) plus 33-22, 76s and 65s at reduced weight.
  - `sb_btn_3bet`: the SB's 3-bet range against BTN. BTN's continuing range
    against the 3-bet is wider than the project's non-positional `vs_three_bet`
    chart, because BTN is in position. It adds 77-55, A9s, KTs, QTs, 98s, AJo and
    KQo at partial weights, plus QQ, AK and KK at 0.15-0.3 as traps.
- **Each street narrows those ranges** by hand class on the actual board:
  - Bettors keep value, draws and some air.
  - Callers keep pairs and draws.
  - A check usually caps a range, with a small weight left for slowplays.
  - Mixed hands get weights between 0.1 and 1.
- **The OOP range in a facing-bet spot is the betting range.** A 33% bet is
  merged: thin value, bluff-catchers that want a showdown, and a few bluffs. A 75%
  bet is polarised: strong value and missed draws.
  - Each spot was checked for its share of air: combos with under 25% equity
    against the other range. A 75% bet has about 25-40% bluffs, and a 33% bet
    about 5-20%.
  - Card-specific draws and flushes use the `@suit` filter.
- **The hero's range** is everything that reaches the decision, not just the
  combos the advisor will be asked about.

## How a spot is scored (`advisor_bench`)

1. **Replay.** The tool deals and replays the script through `GameEngine` to
   reach the hero's river decision. It checks that the script is legal and that
   the hero is the one to act.
2. **Solve.** It builds the `SolveSpot` with `spot_from_view` from the public
   view and the two ranges, then solves it:
   - The tree is `river_default_abstraction()`. Root sizes are the advisor's
     25/50/75/100% ladder plus all-in.
   - `all_in_merge` is set to 1.0, so a size merges into all-in only when it
     takes the whole stack, the same way the advisor lists sizes. Every advisor
     bet or raise then has a root action with the same street total. The tool
     still reports any inexact mapping.
   - The solve runs at least `--min-iterations` and stops once exploitability is
     at most `--target`% of the pot, or at `--iterations`.
3. **Sample.** It picks up to `--combos` hero hands with positive root share. The
   sample is systematic: hands sorted by river strength, taken at evenly spaced
   positions. That keeps value hands, bluff-catchers and air in proportion,
   rather than favouring the highest-weight classes.
4. **Ask the advisor.** For each sampled combo, the tool replays the spot with
   that combo as the hero's hand. The villain gets its first non-conflicting combo
   and the other players get spare cards; neither affects the advisor, which only
   sees the `PlayerView`. It then calls `TrainingAdvisor(seed, rollouts).analyze`.
5. **Score.** The advisor's action maps to a root action: fold, check and call
   directly, all-in to all-in, and a bet or raise by street total, or by nearest
   pot fraction if none matches. Each combo records:
   - `regret_bb = (max_a ev_best[a][h] - ev_best[advisor][h]) / big blind`,
     using the best-continuation EVs from `RootSolution::ev_best`
   - whether the advisor matched the solver's most frequent action
   - the solver's frequency for the advisor's action

Each spot's figures are weighted by the sampled combos' root share,
renormalised. The group tables (by pot type, board family, line, hero role and
overall) average spots with equal weight. Regret is also shown as a percentage of
the pot at the decision, because 3-bet pots are three times larger in big blinds.

## Regenerating the spots

`river_spots.json` is generated by `gen_river_spots.py`, which also works out the expected pot and stacks at the hero's decision without using the engine. `Bench.SpotFileParsesAndEverySpotIsLegal` checks those values against the engine's replay. Run it from the repo root:

```bash
python3 tools/bench/gen_river_spots.py tools/bench/river_spots.json
```
