# Advisor upgrade notes

This file records the decisions, deviations, measurements, unverified items and test changes from the advisor upgrade. The upgrade covers range-based advice, player-type advice and an optional solver mode. It is meant for review later.

Plan summary:
1. Foundations: combos, per-combo equity and a preflop matrix
2. River DCFR solver
3. Benchmark of the current advisor
4. Range model and player types
5. Advisor EV on ranges
6. Preflop upgrades
7. UI
8. Solver mode
9. Validation and docs

Each stage is committed to `main` when its tests pass.

## Environment

- **Machine:** Linux cloud container with 4 cores. GCC 13.3 and clang were used, with CMake 3.28.
- **Configure flags:** GLFW needs `-DGLFW_BUILD_WAYLAND=OFF` here.
- **Packages:** `libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev xvfb`.
- **UI tests:** they run under `xvfb-run -a`.
- **Not tested here:** MSVC, the project's real target. Its `/W4` warnings are approximated by clang with `-Wconversion -Wshadow -Wfloat-conversion` on new files.
- **Baseline at `e531106`:**
  - 355 tests pass in both Release and Debug, in about 4 s and 11 s wall time with `-j 4`.
  - The GCC and clang builds have zero warnings.

## Decisions

1. **The bots are unchanged.** They still use random-hand Monte Carlo equity with `bet_pressure`, as CLAUDE.md section 4 specifies. Keeping them fixed also gives a stable testbed for checking the exploit advice. Only the advisor moves to ranges, and `EquityTable` stays as it is for the bots.
2. **`equity_pct` keeps its meaning:** raw random-hand equity from 5,000 rollouts, as spec section 5 says. It feeds the log's `eq` field. The new `range_equity_pct` is what the UI and rationale show first.
3. **`preferred_size` still uses random-hand equity,** because its strength bars were calibrated on it. Range EV decides whether to bet, and the size rule decides which size.
4. **Player types are generic archetypes** (Baseline, Nit, Station, Maniac) applied to all opponents. No identity for individual seats reaches the advisor, so `PlayerView` is unchanged and the Shifter stays hidden. Reading each seat's type from its stats is future work.
5. **The snapshot's `hint` is still the Baseline advice,** and the archetype lines live in `hint.versus`. Mistakes and the JSONL logs are judged only on Baseline `base_ev`. The schema is unchanged, but the rationale text changes.
6. **Play GTO follows the "Advice:" selector** (default Baseline). In solver mode, once a solve is done, it plays the most frequent solver action, with ties going to the cheaper action.
7. **The solver is our own Discounted CFR,** heads-up and postflop only. The repo has no LICENSE file, so AGPL solvers such as TexasSolver and postflop-solver are not linked. Multiway pots and preflop fall back to the heuristic.
8. **Determinism rules for new code:**
   - No `std::uniform_real_distribution` or `std::shuffle`, because their output differs between standard libraries.
   - Tallies are integers, and floating-point sums run in a fixed order.
   - Sorts break ties by combo index.
   - The bots already use `uniform_real_distribution`, and that is left alone.
9. **Stale-result key:** the solver's key includes a `decision_epoch` owned by the controller, because `new_session()` restarts hand numbers.
10. **Flop equity is exact in the advisor.** The plan proposed sampling the flop as 47 turn cards × 8 river cards. That turned out to be off by up to 1.6 pp on range equity and 5.9 pp on single combos (6 flops × 20 seeds). Exact enumeration of all 1,081 runouts takes about 22 ms on one thread and about 6 ms with automatic threading, which fits the budget, so the advisor uses it.
    - `RangeEquityOptions::flop_rivers_per_turn` now defaults to 0, which means exact; 23 or more is also exact. Smaller values give the sampled mode, kept for cheap test loops. The Stage 1 review pointed out that a default of 8 would silently hand the sampled mode to any caller that passed `{}`.
11. **The flop sampler is "balanced":** one seeded shuffle of the 47 unseen cards, with turn i taking the next K river cards. Each card is a turn once and a river K times. Measured against independent per-turn shuffles, this halves the error (K=8: 0.39 pp rms vs 0.77 pp).
12. **Multiway sampling redraws the whole deal** when two villains' sampled combos collide. Redrawing one villain at a time would bias the joint distribution. For example, villain 1 holding {AA, 72o} and villain 2 holding {AK} should give AA 20% of villain 1's hands; redrawing one villain at a time gives 33%. After 16,384 failed redraws it throws.
13. **Preflop range equity reads the 169 × 169 class matrix,** so it ignores which suits hero's own cards block. The measured bias is about 0.02 pp.
14. **Stage 5: the villains judge Hero's bets against Hero's range as it stands before the bet.** `predict_postflop_response` gets `infer_range_for(view, hero, Baseline, false)` as `bettor_range`, as the plan's batch instructions say. Narrowing it first by Hero's own bet (`bet_likelihood` against each villain, or `raise_likelihood` against the last aggressor when raising) was implemented and measured: 0.886 bb regret and 52% agreement, against 0.867 bb and 54% without it. It was dropped as slightly worse and more expensive; batch D may revisit it.
15. **Stage 5: a called bet or raise assumes one caller and a pot-sized re-raise.** e_call, e_raise and the matched chips are weighted over the villains by their call or raise shares. The re-raise goes to to + (P + a + m_i), capped by both stacks, and Hero calls it or folds, whichever is better. When Hero is all in, the "raise" branch is a call.
16. **Stage 5: preflop all-in only short, cheap, or as the only raise.** With ranges, the single-street model valued a 100 BB UTG open-shove with AA at 258 chips against 93 for a 3.5 BB open, because only the strong part of each range calls a shove and `implied` is capped at half a tiny preflop pot. Preflop, all-in is now advised only with an effective stack of at most 15 BB, within 1.5 pots, or as the only raise left; its `ev` is capped at the advice and `base_ev` keeps the model number. This matches CLAUDE.md section 11's size-rule limit, which had been applied only postflop.
17. **Stage 5: impossible multiway deals fall back to independent villains.** When narrowed ranges can almost never be dealt together (seen in random play: two villains' ranges reduced to the same three JJ combos), `equity_vs_ranges` throws. The advisor then multiplies the heads-up equities instead of failing.
18. **Stage 5: `TrainingAdvisor(seed, rollouts)` keeps working:** it sets `rollouts` and `range_rollouts = max(100, rollouts / 5 * 3)`, so 5,000 gives the default 3,000. The flop is exact, threads are automatic.
19. **Stage 5 batch D: Baseline's postflop defaults are now continue_mult 0.85, value_bet_top 0.28 and bluff_mult 1.2** (were 0.95, 0.33 and 1.0), from the one-round grid search under "Stage 5 tuning". The rule: adopt only if it beats the old defaults by at least 0.03 BB overall and makes no pot-type or hero-role group worse by more than 0.1 BB. It won by 0.124 BB and improved all six groups. The other types' rows are unchanged. The tuning hook is `TrainingAdvisor(seed, options, baseline_params)`, used by `advisor_bench --baseline-params`.
20. **Stage 5 batch D: decision 14 re-measured for raises alone.** Narrowing Hero's range by its raise (not its bets) before the villains answer gave 0.872 BB against 0.867 (old defaults) and 0.751 against 0.743 (new). It was not kept.

21. **Stage 6 batch C: the advisor has its own preflop chart, `advisor_chart` (`src/advisor/advisor_charts.{h,cpp}`).** It is the bots' `chart_response` with the positional answers to 3-bets (as the opener) and 4-bets (as the 3-bettor), and the push/fold tables at an effective stack of 15 BB or less when Hero is first in or faces a lone jam. Where the table says jam, only the all-in gets the approving prior; every smaller raise is rejected. The bots still use `chart_response`, which is untouched.
22. **Stage 6 batch C: players still to act behind a call are weighted by their predicted answer,** with independent continues, every subset enumerated (at most 2^5), and a squeeze counted as Hero folding the call unless calling the squeeze is better. Checks are unchanged. Formulas in `training_advisor.h`, "Players still to act".
23. **Stage 6 batch C: `implied` is 0 preflop.** It was the cause of the remaining size distortion once calls responded to size (must-fix C below). The header already said the chart prior stands in for later streets preflop.
24. **Stage 6 batch C: a short committing raise reads as push/fold up to 15 BB, blending linearly into the commit range by 25 BB** (inference), and a short jam is answered with the push/fold call ranges, blending into the chart answer by 25 BB (prediction). There is no cliff at 15 or 16 BB.
25. **Stage 6 batch C: preflop calls respond to a raise's size in both directions:** f = ratio^-0.5 above the standard size (unchanged), f = ratio^-1 below it, with the extra calls taken from the villain's own folding hands.

## Deviations from the spec or earlier statements

- **What I said earlier:** in the conversation that led to the plan, I said "the GTO bot gets [range equity, exact equity, range-based fold equity] automatically".
- **What changed:** that was reversed (decision 1). Section 4 of the spec fixes the GTO bot's method, and the bots also serve as fixed opponents for measuring the exploit advice.
- **A correction to numbers given earlier:** during planning I quoted 8c8d on Ah9s4c as 65.6% against a random hand, and QhJh on Qd9s8h as 82.3%. Those came from only 4,000 Monte Carlo rollouts, about ±0.75 pp. Exact enumeration gives 63.95% and 82.80%, and 2 million rollouts agree. The comparison still holds: the underpair is about 47% against the BB's defend range and about 23% against the part of it that continues.

- **Stage 5, `BetOutcome`:** its fields are percentages (0-100), like the other `_pct` fields of `HintRecommendation`, not the 0-1 shares the batch instructions described. It also has a fourth field, `equity_raised_pct`, because the raise branch of the EV can't be rebuilt or shown without it.
- **Stage 5, preflop all-in rule:** decision 16 is new, a fix to a clear model bug rather than tuning.

- **Stage 6 batch C, BB 3-bet share against a CO open:** the acceptance band was 8-15% of hands. The advice is 4.5%, exactly the bots' BB-versus-CO 3-bet chart (`vs_open_ranges(BB, CO).raise`, 4.5%), which this stage may not change because the bots share it. `Advisor.BbVsCoOpenThreeBetShareIsPlausible` therefore asserts at least the chart's own share and at most 15%. The band would need a wider advisor-only BB 3-bet chart, which is chart authoring rather than integration.
- **Stage 6 follow-ups, the BB 3-bet band:** resolved by the advisor-only vs-open tables: the BB 3-bets 11.5% against a CO open, and the test asserts 8-15% again.
- **Stage 6 follow-ups, the flop hand check's -17.7:** the reviewer's reference was computed on the Stage 6 ranges. Fix 1 changed the flat and defend ranges in that spot, so the acceptance was checked against the same reference recomputed on the new ranges (-12.1 with the folders out, -14.7 with them dealt), not against -17.7.
- **Stage 6 follow-ups, folders in heads-up scenarios:** the folders' fold parts are dealt only where several opponents contest. With one opponent the exact per-combo average is kept, which leaves the folders' cards out (about +0.8 to -1.3 chips in the two hand checks), because a Monte Carlo there would add several chips of noise to the dominant scenario.
- **Stage 6 batch C, "BTN AJs is about 45% heads-up against the CO":** the plan's figure is not what the range model gives. Heads-up against the Baseline CO open range AJs has 58.3% (all-in, preflop matrix), so the acceptance was read as "within a few pp of 58.3%". The advice is 52.4%, 5.9 pp below, because, predicted against the CO's open, the BB overcalls 24.6% and squeezes 3.6%, and the SB overcalls 8.5% and squeezes 6.2%.
- **Stage 6 batch C, "always opens 3.5 BB":** measured at `d623daf`, the advice opened mostly to the minimum (2 BB), and 3.5 BB with premiums (table below). The cause was the same, calls that did not respond below the standard size, plus `implied`.

## Measured results

### Stage 1 (foundations), measured on the 4-core container

**Preflop matrix** (commit `a22d13b`)
- `tools/gen_preflop_tables` ran 200,000 rollouts per class pair across 14,365 entries, upper triangle plus diagonal, with seed 20261106. It took 196 s on 4 threads.
- Standard error is at most 0.11 pp per entry.
- Sample values:

  | Matchup | Equity |
  |---|---|
  | AA vs KK | 81.97% |
  | 22 vs AKo | 52.79% |
  | QQ vs AKo | 56.62% |
  | AKs vs 72o | 69.05% |
  | KK vs AKs | 65.79% |
  | JTs vs AKo | 40.51% |
  | AKs vs AKs | 49.98% |

- Runs with 1 thread and with 4 threads give byte-identical output.

**Per-combo equity vector** (`hero_vs_each_combo`, Release, all 1,326 combos needed, 60 runs each)

| Street | 1 thread, p50 / p95 | Automatic threading, p50 / p95 |
|---|---|---|
| River | 0.03 / 0.04 ms | (same) |
| Turn | 0.87 / ~1.1 ms | (same); 4 threads 0.44 / 0.55 ms |
| Flop, exact | 21-22 / 23-27 ms | 5.6-5.9 / 6.2-7.3 ms |
| Flop, K=8 | 8.1-8.4 / 9.6-11.5 ms | 2.3-2.6 / 2.6-2.8 ms |
| Preflop (matrix) | 0.04 / 0.06 ms | |

The machine was noisy: runs differed by up to about 20% at p95.

**Multiway Monte Carlo:** 3 players, 3,000 rollouts, 2 pot layers, 0.7-0.9 / 0.8-1.1 ms on 1 thread.

**Flop sampling error against exact,** over 6 flops × 20 seeds:

| K | Uniform range, rms / worst | BB defend range, worst | Single combo, rms / worst |
|---|---|---|---|
| 4 | 0.63 / 2.63 pp | 3.20 pp | 1.39 / 8.1 pp |
| 8 | 0.39 / 1.25 pp | 1.62 pp | 0.88 / 5.86 pp |
| 12 | 0.24 / 0.82 pp | 1.02 pp | 0.59 / 3.96 pp |
| 16 | 0.16 / 0.62 pp | 0.66 pp | 0.39 / 2.41 pp |

**Tests:** 371 pass in Release (about 4 s wall time) and Debug (about 11 s). The GCC and clang builds have zero warnings, and the strict clang check is clean on new files.

**Stage 1 review fixes**
- Tally buffers were sized by the requested thread count rather than the capped one, so a huge `threads` value could allocate gigabytes.
- More than 5 villain ranges now throws, because a 7-way split of 60ths is inexact.
- An exception thrown in a helper thread is now rethrown after the threads join, instead of calling `std::terminate`.

The review checked the rest and found it correct. That covers the combo and triangle indexing, the claim that the balanced sampler gives every combo exactly K rivers per turn, exact mode counting each runout once, the multiway joint sampling and the anti-cheat words. Its brute-force check of the exact flop over all 1,081 combos was within 3e-8. MSVC /W4 was checked by inspection only.

### Stage 2 (DCFR river solver), commit `c5737a8`

**Solver conventions**
- **DCFR iteration indexing:** iteration t updates regrets as R ← R·d(t−1) + r_t, with the sign test on R_{t−1}. The strategy sum is S ← S·((t−1)/t)^γ + s_t. This is equivalent to the paper's end-of-iteration scaling.
  - Each information set is discounted once per iteration, so there is no pruning.
  - Updates alternate between the players.
  - Strategy sums are weighted by the player's own reach.
- **Utility:** u_p is the chips received from the final pot minus the chips added after the root. So u₀ + u₁ = root pot, and folding is worth minus the chips the folder added. Exploitability = (BR₀ + BR₁ − root pot) / 2, as a % of the root pot.
- **Chance nodes** weight each card 1/(N − 4) and zero the reach of hands that hold the dealt card.

**Known answers checked**
- Kuhn poker: player 1's value is −1/18.
- Polarized river toy game, value hands KK against bluffs J9:
  - The bluff share of the betting range is s/(1+2s): 0.25 at s = 0.5 and 1/3 at s = 1.
  - The caller calls 1/(1+s) of the time.
  - The in-position values match the analytic 21.05 and 23.68 chips.
- A turn check-down matches brute-force river enumeration.

**Default river abstraction**
- Root bets: the ladder (25/50/75/100%) plus all-in.
- Deeper bets: 33/75/150% plus all-in.
- Raises: pot plus all-in, with a cap of 2. Sizes merge into all-in at 67% of the stack.
- Tree sizes:

  | Spot | Nodes | Decision nodes | (node, action) slots | Memory |
  |---|---|---|---|---|
  | SPR 3.3 | 63 | 22 | 62 | 0.25 MB |
  | SPR 10 | 96 | 33 | 95 | 0.32 MB |

**Speed and convergence** on Kh9d5c3s2h with 273 vs 227 combos, Release, one thread:

| Spot | ms per iteration | Iterations to 0.5% exploitability | Iterations to 0.3% |
|---|---|---|---|
| SPR 3.3 | 0.245 | 72 | 97 |
| SPR 10 | 0.41 | 138 | 186 |

At SPR 3.3, exploitability is 0.051% of pot after 300 iterations.

**Caveat:** a hand's average strategy at a node it rarely reaches is dominated by early iterations. The bench therefore weights hands by their root share and reach, and doesn't read low-reach nodes literally.

**Memory:** the estimate equals the actual allocation exactly on GCC, because it relies on exact `reserve` and `shrink_to_fit` capacities. The test only requires the estimate to be at least the allocation and no more than 1.2 times it.

**Tests:** 385 pass in Release (about 3 s) and Debug (about 12 s).

**Stage 2 review**
- **No maths defects found.** An independent brute-force solver was run on 80 random spots: river, turn and flop; first to act, facing a bet, facing a raise; all-in runouts; overlapping ranges. Its results matched:
  - Best response values to within 1.3e-7 of the pot.
  - Root EVs to within 2e-7.
  - DCFR training to within 6e-7, except where the per-hand values were round-off noise (see below).
- **Hand-checked toy spot:** pot 100, KK + J9 against 33, one pot-sized bet.
  - Against a uniform strategy: BR₀ = 50, BR₁ = 75, exploitability 12.5%.
  - Solved: J9 bluffs 50% of the time, the caller calls 50%, and EVs are 75 and 25.
  - Both cases match exactly.
- **Facing a bet at the root:** u₀ + u₁ = root pot holds.
- **Fixes:**
  - Bet sizes now compare in double before rounding. MSVC's 32-bit `long` made `lround` unsafe for huge sizes, which could have turned a shove into a minimum bet. A regression assertion was added.
  - solver_cli now computes the memory estimate before printing it.
- **Caveat 1, root action EVs:** `RootSolution::ev` assumes the hand keeps playing its own average strategy after the root action. For actions a hand almost never takes (under 5% of the time), that undervalues the action by 1.7 chips on average and up to 16 chips (0.8 BB) in a pot of 300, and more iterations don't fix it. For actions taken at least 5%, the gap is 0.06 chips. **Stage 3 adds a best-continuation EV and measures regret on it.**
- **Caveat 2, zero-reach hands:** where a hand faces no opponent combos with any reach, round-off of about 1e-17 drives an arbitrary strategy for that hand. It doesn't affect values, but displayed frequencies must be weighted by reach.

### Stage 3 (river bench): the baseline to beat

**The bench**
- `tools/advisor_bench` uses `tools/bench/river_spots.json`, produced by `gen_river_spots.py`: 36 river spots in 100 BB pots.
  - 3 pot types: BTN vs BB, CO vs BTN, and SB 3-bets BTN.
  - × 4 board families: dry, wet, paired, monotone.
  - × 3 lines: check-check, bet then check, bet-bet.
  - Hero is either first to act out of position, or in position facing a 33% or 75% bet.
- The ranges are hand-written assumptions, not solved. Each bettor's share of air was checked to be plausible: 27-41% for 75% bets and 4-20% for 33% bets.
- Every spot is solved to ≤ 0.3% of pot exploitability, with at least 2000 iterations. It was 200 at first; the Stage 3 review showed that off-path `ev_best` values hadn't converged at 200, with single combos moving by up to 2.9 BB.
- **Hero combos:** up to 80 per spot, sampled evenly across river hand strength.
- **Regret:** the best `ev_best` minus the `ev_best` of the advisor's action, in BB. `ev_best` is the best-continuation EV.
- **Size mapping:** the solver tree uses `all_in_merge = 1.0`, so every advisor size maps exactly.
- **Cost:** a full run takes 2.1 s, of which 1.0 s is solving. The advisor takes p50 0.35 ms and p95 0.40 ms per river decision.

**Baseline: the current random-hand advisor, at 2000 or more solver iterations**

At 200 iterations the first measurement had been 1.992 BB overall regret and 28% agreement.

| Group | Regret (BB) | % of pot | Regret > 0.5 BB | Regret > 1 BB | Agrees with solver's top action | Solver's frequency for advised action | Advised action used < 10% |
|---|---|---|---|---|---|---|---|
| **Overall (36)** | **1.946** | 5.38 | 47% | 39% | 28% | 28% | 69% |
| BTN vs BB | 0.939 | 4.38 | 41% | 28% | 27% | 26% | 70% |
| CO vs BTN | 1.422 | 5.73 | 49% | 40% | 26% | 27% | 71% |
| SB vs BTN 3-bet | 3.478 | 6.04 | 52% | 48% | 32% | 31% | 65% |
| Dry / wet / paired / monotone | 1.68 / 1.68 / 2.26 / 2.17 | | | | 30 / 21 / 33 / 30% | | |
| OOP first to act | 0.966 | 3.59 | 38% | 25% | 24% | 23% | 71% |
| IP facing 33% | 1.856 | 5.41 | 43% | 35% | 31% | 31% | 65% |
| IP facing 75% | 3.016 | 7.15 | 61% | 55% | 30% | 30% | 69% |

By line, regret is 1.38 BB for check-check, 1.27 for bet then check, and 3.18 for bet-bet. A full run takes 10.4 s, of which 9.4 s is solving.

**Stability:** `--stability` re-solves each spot at 4000 iterations. The mean change in a combo's regret is 0.011 BB, overall regret moves by 0.002 BB, and the largest move for one spot is 0.028 BB.

**Patterns, consistent with random-hand equity overrating hands on the river**
- **It bets too often when first to act, and too big.** On one monotone board the solver checks 92% and the advisor bets 84%.
- **It raises with bluff-catchers when facing bets.** On a paired board in the 3-bet pot it raises 67% where the solver raises 2%, and QQ raises for −7.2 BB where calling is worth +8.8 BB. On one wet bet-bet line it shoves every hand where the solver folds 49%.
- **It rarely folds.** One spot has the solver folding 73% and the advisor 21%.
- **Agreeing is nearly free:** when the advisor matches the solver's top action, its mean regret is 0.002 BB. That supports measuring regret on `ev_best`.
- **The plan's acceptance target for Stage 5:** mean regret ≤ 1.46 BB (0.75 × 1.946), and agreement ≥ 38%.

**Stage 3 review fixes**
- `advisor_bench` now uses at least 2000 solver iterations, and exits with status 3 if any advisor action maps inexactly to a solver action.
- `spot_from_view` caps both street bets at what the shorter player can reach and drops the uncalled excess. This was latent: every bench stack is 2000, but uneven stacks in the trainer would have solved the wrong spot.
- A comment notes that `raises_made` counts short all-in raises.
- Two spot notes were corrected; the numbers didn't change.

### Stage 4 (range model and player types)

**What it adds:** `src/range/range_model.{h,cpp}`, with the model documented in the header, and `resize_range` in `preflop_charts`. The bots never call either.

**Refinements to the plan's model**
- **`hs` (hand strength)** is the share of live combos beaten, with ties counted as half. It ignores the hand's own cards blocking other combos, so equal hands tie exactly.
- **`top_share`** is the midpoint of the hand's tie group within the range.
- **Bet likelihood:** P = p_v + (1 − p_v)·x, where p_v is the value part.
  - x is `draw_bet` for strong draws, a medium rate for middling hands, and a bluff weight b for air.
  - b is calibrated so the air share of the polarised part is m·r/(1+m·r), with r = s/(1+s) and m = `bluff_mult`. A test checks this to 1e-6.
- **Bluff raises come out of the continue budget,** recomputed once. Without that, the Maniac folds about 7 pp less than intended.
- **New: checking to the previous street's aggressor** uses 1 − `lead_share` (0.15) × P(bet). Without it, a BB check-call stripped the aces from its range on Ah9s4c, and 88's equity rose from 51.8% to 61.5% instead of falling to 43.1%.
- **Preflop chart answers are probabilities:** a 3-bet weighs 1 − slowplay, and a flat call keeps the 3-bet hands at the slowplay weight.
- **Preflop gaps filled:**
  - SB flats use the in-position call chart.
  - The BB iso-raise range is Premium plus Strong.
  - A BB check is exempt from the range cap.
  - Calls of 4-bets use `call_3bet_width`.
  - Every all-in over 15 BB uses the commit range (JJ+ and AK).
- **Temporary short-stack formulas,** until Stage 6 adds push/fold tables:
  - Shoving ≤ 15 BB: the top clamp(400/BB, 5, 100)%, halved when facing a raise.
  - Calling such a shove: the top clamp(250/BB, 8, 100)%.
- **Fallback:** a range falls back to the last action's likelihood only when it drops below 1% of combos *and* below 5% of its prior mass. This keeps small but legitimate ranges, such as a Nit's 4-bet range. (Superseded by the review fixes below.)

**Speed:** `infer_ranges` on a river view with bets and calls on every street takes p50 0.5 ms and p95 0.6 ms with 2 opponents, and 0.84 / 1.22 ms with 5. `street_strength` takes about 0.15 ms.

**Real-number spot checks** (the implementer's judgment, not solver-verified)

On K72r, BTN vs BB, after a flop c-bet is called and the turn is checked, the viewer holds AQ. Responses are fold / call / raise to a bet of 33 / 75 / 125% of the pot:

| Type | Range size | At 33% | At 75% | At 125% | AQ's equity against the range |
|---|---|---|---|---|---|
| Baseline | 218 combos | 28 / 62 / 10% | 46 / 44 / 10% | 58 / 32 / 10% | 50.3% |
| Nit | 82 | 47% fold at 33% | 60% fold at 75% | 69% fold at 125% | 30.9% |
| Station | 591 | 4% fold at 33% | | 31% fold at 125% | 52.1% |
| Maniac | 142 | 20% fold, 25% raise | | | 63.5% |

- **A 3-bet pot on Js8s3d** looks plausible: the caller's range is 10% two pair or better, 44% one pair, 13% draws and 32% air.
- **8c8d on Ah9s4c:** 51.8% against the Baseline BB defend range (with traps), and 43.1% after the BB check-calls a third-pot bet.

**Known weaknesses, left for Stage 5 tuning and logged**
1. **Raise share was flat** at `raise_share` for every bet size. **Fixed:** the cap is now `raise_share` × clamp(1.5/(1+s), 0.4, 1.25), so it is ×1.13 at 33% pot and ×0.67 at 125%. Both inference and prediction use it. On K72r the Baseline now raises 11 / 9 / 7% against bets of 33 / 75 / 125% of the pot, and the Maniac 27 / 21 / 17%.
2. **Few pure bluffs after three barrels:** only 11% of the Baseline's river betting range is no-pair air. The model's bluffs are the bottom of its own betting range, which by the river is mostly weak pairs.
3. **The Station keeps 46% air after calling two barrels,** because the 0.95 continue cap lets it call almost anything at small sizes. That is roughly what a station does, but it's extreme.
4. **The Maniac bets almost its whole range at 33%,** because its value threshold, 0.72, overlaps the air region.
5. **The 33%-bet air-share test passes by only 0.011.**
6. **Untuned constants:** draw equity thresholds, the medium-hand bet rate, the bluff-raise rate, `lead_share` and the short-stack formulas.

**Tests:** 410 pass, which is 393 plus 15 new tests and 2 disabled reports (`RangeModelReport.DISABLED_ThreeSpots` and `DISABLED_Timing`). Each new test takes ≤ 0.03 s.

### Stage 4 review fixes

The Stage 4 review found that value was judged only within the actor's own range, so a capped range read its medium hands as value and Hero's equity against aggression came out far too high. The model is documented in full in `range_model.h`; the changes are:

**Decisions**
- **Bets and raises are judged against the opponents.** A combo's standing u is the largest top_share it holds in any live opponent's current range. Value means more than half the equity against the callers, the top C of a range, so u ≤ C/2, with C a standard player's continue share Cref(s) = clamp(0.95/(1+s), 0.10, 0.95) for every type. A type's aggression enters as a shift of value_bet_top − 0.33. The walk now tracks every dealt player's range, the viewer's included, all with the one params set.
- **Margins, tuned on the reviewer's spots** (judgment, not solver-verified):
  - River bets need u ≤ 0.6 × Cref/2 (ahead of 70% of the calls).
  - Raises assume the bettor faces a 3x raise, s_r = 2s/(1+2s), and continues with 0.5 × Cref(s_r); a value raise is ahead of half of that.
  - Bluff raises are calibrated to the value-raise mass like bets (bluff_mult × s_r/(1+s_r)), from the bottom of the raiser's own range.
  - The previous street's aggressor bluffs 4× the river ratio on the flop and turn (c-bets and barrels). The bluff weight is capped at 0.9, so a check never removes all air.
  - Bets under half pot slowplay less: sp × s/0.5.
- **Edges are a cubic smooth step** (zero beyond ±2w) instead of a logistic. The logistic's tails over many weak hands outweighed a narrow value region: With everything else as now, the logistic gives AQ 44.6% against the river check-raise (x/c flop, x/x turn) and 61.1% against the flop check-raise; the step gives 27.5% and 56.8%.
- **`predict_postflop_response` takes `bettor_range`,** and the likelihood functions take the opponent or bettor range. Null keeps the old own-range rule, documented as overstating raises against a strong bettor.
- **Limpers facing the first raise** use their own answer: calls are the strongest 0.6 × flat_width of the limp window, plus traps at sp; raises are the traps and the strongest 0.1 × three_bet_width of the window. `predict_preflop_response` has `villain_limped`.
- **Fallback:** the product is trusted at ≥ 1% of combos or ≥ 0.5% of the prior (was 5%). Otherwise the likelihood goes on the street-start range, then the preflop range, then every combo.
- **Width caps limit growth only** and never cut below the chart: cap = max(max_range_pct, chart %) when the width is above 1.
- **A short all-in is not a new raise level** (the engine's full-raise rule). The first raise always counts.

**Deviations from the plan's table:** `max_range_pct` for the Station goes from 70 to 80 and for the Maniac from 75 to 90, so they open wider than the 75% heads-up BTN chart. The Nit's 15 now only limits growth, and the Nit is narrower through its widths alone: its CO/BTN/SB opens are 16.4/27.5/25.6% instead of 14.2% each.

**Measured** (Baseline, AcQc on the BTN, Qs9h4d2c7s, BTN opens and the BB calls; old → new):

| Spot | Old | New | Plausible |
|---|---|---|---|
| Flop check-raise | 72.1% | 56.8% | 45–60% |
| River check-raise, after x/c flop and x/x turn | 91.2% | 27.5% | ≤ 35% |
| River check-raise, after x/c flop and turn | 60.2% | 25.4% | ≤ 35% |
| River lead of 75%, after x/c flop and x/x turn | 86.3% | 72.8% | 55–65% (**still high**) |
| 3-bet range c-bet at 33% on K83r | 59% | 89% | ≥ 85% |
| BTN c-bet on K72r vs BB at 33/75/125% | 56/43/37% | 71/63/59% | 60–80% at 33% |
| 8c8d on Ah9s4c after BB check-calls 33% | 43.5% | 43.4% | 40–48% |
| Baseline UTG limp-call of an iso | 4 combos | 14.3 | |
| HU BTN open, Nit/Base/Station/Maniac | 14.2/75/69.8/75% | 45.1/75/79.8/89.1% | |

**The river lead stays at 72.8%, and that was accepted.** The 55–65% band was the reviewer's estimate, not a solver result. In this model the BB 3-bets AQ and KQ preflop, so every one-pair hand it leads for value is worse than AQ. Its value range is sets, two pair and straights, plus thin queens, with about 30% bluffs. Tightening river value further only got to about 70%. Stage 5's bench measures whether this hurts the advice.

- **The river lead is still high:** the BB 3-bets AQ and KQ, so every one-pair hand it leads for value is worse than AQ, and balanced bluffing (30% at 75% pot) sets a floor. Tightening river value further only reached about 70%.
- **Check ranges:** a BB that checks the flop, turn and river keeps 156 of 453 combos (141 before). A BTN that checks back all three streets keeps 29 of 610 combos, mostly ace-high showdown value (107 before), because its c-bets and barrels now take most of its air.
- **Viewer card removal** moves equity by at most 1.5 pp in the reviewer's three spots (≤ 0.8 pp before).
- **Speed:** `infer_ranges` on a river view, over two runs: p50 0.74–0.79 ms and p95 0.88–1.37 ms with 2 opponents; p50 1.11–1.18 ms and p95 1.28–1.82 ms with 5 (was p95 1.29). Every dealt player is now tracked and judged against the others.

### Stage 5 batch A (Baseline advisor on ranges), uncommitted, on top of `f9d64e5`

**The model** is documented in full in `training_advisor.h`. In short:
- Fold is 0. Check is e_L × contestable. Call is e_L × (contestable + to_call) − to_call, where e_L is the layered range equity, each pot layer against its own villains' ranges.
- A bet or raise of a chips is F·P·k + P_call·V_called + P_raise·V_raised, using each foldable villain's `predict_postflop_response` (or `predict_preflop_response`). The terms are in decision 15.
- The chart prior, the cheap-call rule (inside `chart_response`), the size rule and its limits, and the `ev`/`base_ev` split are unchanged.
- Removed: `bet_pressure`, the call floors, the exponents, the overbet term, the multiway averaging, and the unopened 15% model.

**River bench** (`advisor_bench`, defaults, 2000 or more solver iterations), against the Stage 3 baseline:

| Group | Stage 3 regret (BB) | Now (BB) | Stage 3 agree | Now agree |
|---|---|---|---|---|
| **Overall (36)** | **1.946** | **0.867** | **28%** | **54%** |
| BTN vs BB | 0.939 | 0.324 | 27% | 59% |
| CO vs BTN | 1.422 | 0.530 | 26% | 53% |
| SB vs BTN 3-bet | 3.478 | 1.748 | 32% | 51% |
| Dry | 1.68 | 0.700 | 30% | 50% |
| Wet | 1.68 | 0.271 | 21% | 63% |
| Paired | 2.26 | 1.197 | 33% | 54% |
| Monotone | 2.17 | 1.300 | 30% | 50% |
| OOP first to act | 0.966 | 0.450 | 24% | 40% |
| IP facing 33% | 1.856 | 0.433 | 31% | 67% |
| IP facing 75% | 3.016 | 1.719 | 30% | 55% |
| Check-check line | 1.38 | 1.118 | | 54% |
| Bet then check | 1.27 | 0.741 | | 49% |
| Bet-bet | 3.18 | 0.744 | | 59% |

- Overall regret is 0.45× the baseline (target ≤ 0.75×), and agreement is +26 pp (target +10 pp). Regret above 1 BB falls from 39% to 17% of hands, and regret as a share of the pot from 5.38% to 2.52%.
- **Worst spots:** IP facing a 75% bet in the 3-bet pot. On monotone-xx regret is 5.9 BB and on paired-bx 4.9 BB, and on dry-xx it is 3.9 BB. The solver calls 83-92% there; the advisor folds 32-57% and raises 1-37%.
  - The cause shows on dry-xx (Kc8h3d2s6c): AK, KQ, KJ, QQ, JJ, TT and 99 raise 75%, AQ and AJ fold, and the solver calls them all.
  - The model predicts that a betting range facing a raise continues at about 0.95 × MDF of its own range. So JJ's raise wins about 46% of the time against a lead the bench's ranges make polarised, and AQ can't beat the price.
  - This comes from the range model's response to raises and its few river bluffs (Stage 4 weakness 2), not from the advisor arithmetic. It is left to batch D's tuning.
- **OOP first to act still bets too often:** 13% checks against the solver's 62% on btn_bb-dry-xx, and 44% against 90% on btn_bb-monotone-xx. Fold equity against MDF-based continues beats checking's plain e × pot.
- **Per hand:** the advisor takes p50 2.25 ms and p95 3.23 ms per river decision on the bench (Stage 3: 0.35 / 0.40 ms).

**Hint latency** (`Advisor.DISABLED_LatencyByStreet`, Release, default options, ms).
- Random 6-max play with mostly checks and calls, so nearly every postflop spot is 3 or more ways.
- Plus 300 heads-up spots checked to on each street.

| Street | Automatic threads, p50 / p95 / max | 1 thread, p50 / p95 / max |
|---|---|---|
| Preflop (2014, mostly multiway) | 3.9 / 5.0 / 32 | 5.8 / 7.8 / 43 |
| Flop, 3+ way (1774) | 6.8 / 13.0 / 25 | 11.4 / 29.2 / 61 |
| Flop, heads-up (300) | 5.7 / 6.9 / 16 | 16.5 / 20.1 / 27 |
| Turn, 3+ way (1588) | 5.6 / 8.8 / 46 | 6.6 / 12.1 / 119 |
| Turn, heads-up | 2.1 / 2.5 / 4.3 | 2.1 / 2.9 / 11 |
| River, 3+ way (1434) | 5.5 / 11.3 / 334 | 6.2 / 18.7 / 844 |
| River, heads-up | 1.8 / 2.4 / 4.5 | 1.8 / 2.4 / 4.4 |

- **Targets:** the multiway flop is well within 50 ms (worst case 25 ms automatic, 61 ms on 1 thread). Heads-up turn and river are within 10 ms. The multiway river p95 is 11.3 ms with automatic threads, just over 10 ms.
- **Spikes:** the multiway maxima of 334 ms and 844 ms come from narrowed ranges that are nearly impossible to deal together, where `equity_vs_ranges` redraws thousands of times before succeeding or giving up (decision 17). They only appeared in random play's absurd lines, such as six-way min-raised 4-bet pots.
- **Where the time goes** on turn and river (callgrind, 1 thread):
  - 31% multiway Monte Carlo.
  - 30% the two range walks (`infer_ranges` and `infer_range_for`); each walk recomputes `street_strength` on every street, which allocates inside `read_hand`.
  - 20% random-hand equity.
  - 13% the bet responses.
  - Sharing one walk, or caching `street_strength`, would roughly halve the walk cost but needs a change in `src/range`.

**`AdvisorSizing.EverySizeIsUsedAndAdviceStaysSane`** distribution over 1500 heads-up spots checked to:

| Run | Bets | Advised 25 / 50 / 75 / 100% | Model alone 25 / 50 / 75 / 100% |
|---|---|---|---|
| Old advisor, 300 rollouts | 911 | 204 / 397 / 229 / 81 | 290 / 117 / 99 / 405 |
| New, fast options {300, 500, 1, 1, false} | 883 | 245 / 341 / 216 / 81 | 281 / 205 / 135 / 262 |

The model alone now spreads over sizes rather than piling on pot-sized bets. No bound was relaxed.

**Test suite:** 424 tests pass in Release (420 + 4 new; one new disabled report). Release takes about 6 s and Debug about 24-25 s, against about 12 s before. The two loop tests account for most of it: `EverySizeIsUsedAndAdviceStaysSane` takes 2.3 s in Release and 17 s in Debug (0.1 s before), and `FieldsInRangeAndBestActionLegalAcrossRandomStates` 2.0 s and 10 s. The cost is the range walks and per-combo vectors, even with fast options. The GCC Release and Debug builds and the clang build have zero warnings, and `strict.sh` is clean on `training_advisor.cpp`, including its six earlier sign-conversion warnings.

**Hand-checked numbers** (pot 200, 1900 behind):
- **Royal flush on the river, checked to, betting 200:** F = 52.6%, raises 7.5%, calls 39.9%. A call wins 200 + 400 − 200 = 400. A pot-sized re-raise goes to 800 and Hero's call of it wins 200 + 1600 − 800 = 1000. EV = 0.526 × 200 + 0.399 × 400 + 0.075 × 1000 = 339.8, as listed.
- **The same bet with 88 on Kd8s3c (a set):** 97.2% against the calls and 94.9% against the raises; EV 446.4 against 194.2 for checking.
- **K9o facing 150 into 200 on the flop:** 74.4% against the estimated range and 85.9% against a random hand. The advice is to raise to 400.

### Stage 5 batch B (archetype lines), uncommitted, on top of `4b7b985`

**What it adds:** `hint.versus[0..2]` for Nit, Station and Maniac when `AdvisorOptions::archetype_lines` is on (the default). The model is documented in `training_advisor.h` under "Archetype lines". No change in `src/range`, the bots or the JSONL schema; mistakes stay on Baseline `base_ev`.

**Decisions**
- **Same pipeline per line.** Each line runs `infer_ranges(view, type)` for every opponent and that type's `archetype_params` for their answers, through the same `evaluate_line` code as Baseline (check/call/raise EVs, chart prior, size rule, fit, all-in rules). Each line has its own `ev` with fit; there is no per-line `base_ev`.
- **Shared per-combo vector.** E is computed once over the union of all four lines' combos. Each entry depends only on its own combo (river and turn exact, flop sample seeded per decision, preflop matrix), so Baseline is bit-identical with lines on or off (`BaselineHintUnchangedByArchetypeLines`; bench unchanged).
- **Bettor range = Hero's range as Baseline reads it,** for every line. The types differ in how they answer a bet, not in how they read Hero, and a fixed bettor range keeps each line's difference to the opponents' answers. (Not compared against a per-type reading of Hero.)
- **Value-size shift:** a value bet is the size rule's own "solid" or "very strong" hand (`SizeChoice::value`). Only then, and only where betting already beats the passive options, does the chosen rung move by `value_size_shift` (Nit −1, Station +1, Maniac 0), clamped to the ladder. A shifted size that would take the whole stack is used only when all-in is in range (≤ 1.5 pots); otherwise the unshifted size stays. Probes, thin value and draws never shift. Preflop has no shift (EV picks the size there).
- **Preflop chart prior: full weight in the lines** (`kArchetypePriorScale = 1.0`). Measured over all 169 classes in six spots (lines that change Fold/Call/Raise against Baseline at full prior):

| Spot | Line prior 1.0 (Nit/Station/Maniac) | Line 0.5, Baseline 1.0 | Both 0.5 (control) |
|---|---|---|---|
| BTN first in | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| CO first in | 0 / 0 / 0 | 11 / 1 / 0 | 11 / 1 / 0 |
| SB first in | 0 / 2 / 1 | 80 / 4 / 4 | 62 / 14 / 15 |
| BB vs CO open | 66 / 0 / 0 | 1 / 23 / 36 | 36 / 20 / 5 |
| BTN vs CO open | 2 / 3 / 11 | 66 / 152 / 154 | 88 / 2 / 0 |
| SB vs BTN open | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

  - At half weight the lines 3-bet 152 (Station) and 154 (Maniac) of 169 classes on the BTN against a CO open, but only 2 and 0 against a Baseline at the same half weight: the change comes from weakening the prior, not from the type. The preflop model alone is loose (it opens K4o, Q7o, T7o and 65o from the BTN at positive raw EV, and 3-bets nearly everything), so a weaker prior lets that looseness through and would teach it as an exploit.
  - The exploit differences are small next to the prior. BTN first in with K4o, a 2.5x open to 40: model EV vs Nit blinds −16.7 + 30 prior = +13.3 raw, vs Baseline +7.6 raw, so the Nit edge is 5.7 chips (0.29 BB) against a 30-chip (1.5 BB) prior for a rejected raise. Stealing K4o vs Nits would need a prior weight below 0.44, where Baseline-strength looseness starts too (Baseline's own K4o flips below 0.25).
  - At full weight the lines still differ where the type matters most: BB vs a Nit's CO open drops 66 light 3-bets to folds (the Nit folds 55% to the min 3-bet against Baseline's 72%); BTN vs a Maniac's CO open 3-bets 11 more hands (A2o-A9o, K9s, A7s, KTo).
  - **Side finding, Baseline (not changed here):** Baseline BB 3-bets nearly every hand against a CO open, including 72o (raw raise EV about +50 from 72% folds to a min 3-bet, against a 35-chip prior). That is batch A's preflop model plus the non-positional vs-3-bet charts; Stage 6's charts should be checked against it.
- **`why`:** one sentence from the numbers, labels shortened ("BET 150"), at most 140 characters (tested on every random state). Cases: same action ("Same as Baseline: folds 44% to BET 50 (Baseline 24%); 11% equity when called." or range equity vs pot odds); a bet where Baseline is passive (fold share up by ≥ 5 pp → "betting profits", else equity when called → "bets thinner for value"); passive where Baseline bets (continue share, raise share, or equity when called at Baseline's size); both betting at different sizes ("value bets go bigger/smaller" when shifted); otherwise range equity against pot odds.

**Performance.** The first version (serial, full Monte Carlo per line) took river 3+ way p95 46 ms and turn 29 ms. The cost was not the walks (about 1.2 ms each) but the Nit line's multiway Monte Carlo: narrowed Nit ranges collide, and `equity_vs_ranges` redrew up to 1.3 million deals (25-120 ms) in about 10% of multiway river spots. Fixes, all inside the advisor:
- The walks (four types plus Hero's range) and then the four lines run on threads (`run_tasks`), unless `threads == 1`; results are bit-identical either way (`ThreadCountDoesNotChangeTheHint` now compares the lines too).
- Postflop the lines' multiway Monte Carlo uses a third of `range_rollouts` (1000 by default; Baseline's seed, so the first chunk's deals match), and fewer where a 512-deal trial finds most deals clash: rollouts = clamp(150,000 × acceptance, 100, 1000). If no trial deal fits, the line uses the independent-opponents product directly. Preflop keeps the full count.
- Rejected alternative: a calibrated independence product (line product + Baseline's Monte Carlo minus Baseline's product) for all lines. Against a 40,000-rollout reference on 477 multiway turn/river line spots its error was mean 1.6 pp, p95 6.1 pp, max 23 pp; plain 3000-rollout Monte Carlo was 0.3 / 1.1 / 2.2 pp.

**Hint latency** (`Advisor.DISABLED_LatencyByStreet`, Release, default options, ms, p50 / p95 / max; this container was faster than batch A's, so the "off" rows are the comparison):

| Spot | Lines off, auto threads | Lines on, auto threads | Lines off, 1 thread | Lines on, 1 thread |
|---|---|---|---|---|
| Preflop 3+ way (2010) | 2.6 / 3.4 / 7.7 | 5.9 / 8.1 / 20 | 4.1 / 5.4 / 17 | 12.6 / 17.0 / 47 |
| Flop 3+ way (1774) | 4.7 / 8.5 / 20 | 8.0 / 11.4 / 27 | 8.1 / 20.1 / 43 | 19.6 / 27.1 / 56 |
| Flop heads-up (300) | 3.8 / 4.3 / 5.0 | 4.7 / 5.6 / 7.4 | 11.7 / 14.0 / 19 | 14.3 / 16.3 / 24 |
| Turn 3+ way (1588) | 3.9 / 6.2 / 33 | 5.6 / 11.3 / 42 | 4.8 / 8.9 / 84 | 10.3 / 20.0 / 97 |
| Turn heads-up | 1.5 / 1.6 / 2.0 | 2.1 / 2.6 / 4.5 | 1.5 / 1.8 / 3.7 | 2.7 / 3.1 / 4.6 |
| River 3+ way (1434) | 3.9 / 7.7 / 254 | 5.3 / 14.7 / 258 | 4.5 / 13.6 / 624 | 10.2 / 27.6 / 633 |
| River heads-up | 1.3 / 1.4 / 1.7 | 1.7 / 2.0 / 3.0 | 1.3 / 1.5 / 2.2 | 2.9 / 3.3 / 4.9 |

- Targets with automatic threads: multiway flop worst case 27 ms (≤ 50), turn p95 11.3 ms and river p95 14.7 ms (≤ 15). The river maxima (about 250 ms) are Baseline's own redraw spikes (decision 17), present with lines off too. On one thread the targets are missed (river 3+ way p95 27.6 ms).

**Three example spots** (heads-up, Kd8s3c2hJd, pot 200, 1900 behind; Hero is the BB; default options, seed 60)

| Spot | Baseline | vs Nit | vs Station | vs Maniac |
|---|---|---|---|---|
| 5c4d, flop, first to act (air) | BET 50 (24% folds) | BET 50 | CHECK | CHECK |
| KsQh, flop, first to act (top pair) | BET 100 | BET 50 | BET 150 | BET 100 |
| As3d, river, faces a pot bet after two checked streets (third pair) | CALL 200 (39% vs range, 33% odds) | FOLD | FOLD | CALL 200 |

- 5c4d, Station: "vs Station: continues 95% vs BET 50 (Baseline 76%): too little fold equity, so CHECK." Nit: "Same as Baseline: folds 44% to BET 50 (Baseline 24%); 11% equity when called." (stab EV 76 vs check 33).
- KsQh, Nit: "vs Nit: BET 50, not BET 100: folds 53% to BET 100 (Baseline 37%), so value bets go smaller." Station: "vs Station: BET 150, not BET 100: folds 20% to BET 150 (Baseline 46%), so value bets go bigger."
- As3d facing pot, Nit: "vs Nit: 22% vs the range (Baseline 39%) against 33% pot odds, so FOLD." Station: "vs Station: 29% vs the range (Baseline 39%) against 33% pot odds, so FOLD." Maniac: "Same as Baseline: 54% vs the range (Baseline 39%) against 33% pot odds." The Station folds because its river bets after two checks are mostly value in this model (bluff_mult 0.4); a real station's bets may be lighter.
- Also: As3d first to act on the river is thin value. Baseline and Station BET 50; Nit CHECKs ("vs Nit: only 36% equity when called at BET 50 (Baseline 72%), so CHECK."); Maniac CHECKs (continues 86%).

**Tests:** 428 pass in Release (424 + 4 new) and Debug. Release suite about 6 s, Debug 23.7 s. `ArchetypeLinesAreLegalAndDeterministic` takes 1.6 s in Release and about 10 s in Debug (3 random games, fast options with lines on). GCC Release/Debug and clang build with zero warnings; `strict.sh` clean on `training_advisor.cpp`. River bench unchanged: overall 0.867 BB, 54% agree.

### Stage 5 tuning (batch D), uncommitted, on top of `a052525`

**The hook.** `TrainingAdvisor(seed, options, baseline_params)` runs the Baseline line (the hint's own fields: the opponents' ranges, their answers, and Hero's range as they read it) with the given `ArchetypeParams` instead of `archetype_params(Baseline)`; the Nit, Station and Maniac lines keep their rows. It is a constructor rather than an `AdvisorOptions` field because a new trailing field makes every brace-initialised `AdvisorOptions{...}` warn under `-Wmissing-field-initializers`. `advisor_bench --baseline-params C,V,B` uses it (C = `continue_mult`, V = `value_bet_top`, B = `bluff_mult`, the other Baseline fields unchanged). The trainer never calls it. `--baseline-params 0.95,0.33,1.0` reproduced the old defaults exactly (the same tables; the CSVs differed only in the timing column).

**Grid, one round only** (river bench, defaults otherwise, 2000 or more solver iterations; 27 runs, 4 at a time, about 2 minutes). Sorted by overall regret, BB. The old defaults are in bold, the adopted row in italics.

| continue_mult | value_bet_top | bluff_mult | Overall | Agree | BTN vs BB | CO vs BTN | SB vs BTN 3-bet | IP vs 33% | IP vs 75% | OOP first |
|---|---|---|---|---|---|---|---|---|---|---|
| *0.85* | *0.28* | *1.2* | *0.743* | *56%* | *0.288* | *0.435* | *1.507* | *0.354* | *1.464* | *0.410* |
| 0.85 | 0.33 | 1.2 | 0.758 | 57% | 0.284 | 0.384 | 1.604 | 0.426 | 1.391 | 0.456 |
| 0.95 | 0.28 | 1.2 | 0.789 | 56% | 0.328 | 0.398 | 1.642 | 0.484 | 1.484 | 0.399 |
| 0.85 | 0.40 | 1.0 | 0.797 | 52% | 0.327 | 0.398 | 1.665 | 0.389 | 1.422 | 0.579 |
| 0.95 | 0.33 | 1.2 | 0.797 | 57% | 0.327 | 0.495 | 1.569 | 0.445 | 1.500 | 0.445 |
| 0.85 | 0.40 | 1.2 | 0.802 | 53% | 0.335 | 0.349 | 1.722 | 0.493 | 1.337 | 0.576 |
| 0.85 | 0.28 | 1.0 | 0.819 | 55% | 0.296 | 0.524 | 1.638 | 0.373 | 1.671 | 0.414 |
| 0.95 | 0.40 | 1.2 | 0.841 | 51% | 0.486 | 0.373 | 1.663 | 0.562 | 1.397 | 0.563 |
| 0.95 | 0.28 | 1.0 | 0.843 | 56% | 0.315 | 0.488 | 1.725 | 0.379 | 1.755 | 0.394 |
| 0.85 | 0.33 | 1.0 | 0.859 | 53% | 0.291 | 0.486 | 1.799 | 0.517 | 1.587 | 0.472 |
| **0.95** | **0.33** | **1.0** | **0.867** | **54%** | **0.324** | **0.530** | **1.748** | **0.433** | **1.719** | **0.450** |
| 0.95 | 0.40 | 1.0 | 0.871 | 52% | 0.478 | 0.413 | 1.720 | 0.402 | 1.613 | 0.597 |
| 1.05 | 0.28 | 1.2 | 0.876 | 56% | 0.369 | 0.578 | 1.681 | 0.441 | 1.641 | 0.547 |
| 0.95 | 0.33 | 0.8 | 0.889 | 54% | 0.313 | 0.537 | 1.818 | 0.345 | 1.813 | 0.510 |
| 0.85 | 0.33 | 0.8 | 0.893 | 53% | 0.294 | 0.484 | 1.901 | 0.603 | 1.591 | 0.485 |
| 0.85 | 0.40 | 0.8 | 0.908 | 50% | 0.325 | 0.452 | 1.948 | 0.342 | 1.748 | 0.634 |
| 1.05 | 0.28 | 1.0 | 0.930 | 55% | 0.360 | 0.613 | 1.819 | 0.346 | 1.922 | 0.524 |
| 0.95 | 0.28 | 0.8 | 0.937 | 55% | 0.331 | 0.517 | 1.962 | 0.599 | 1.810 | 0.401 |
| 1.05 | 0.33 | 1.2 | 0.939 | 55% | 0.395 | 0.644 | 1.778 | 0.518 | 1.708 | 0.592 |
| 1.05 | 0.28 | 0.8 | 0.962 | 55% | 0.377 | 0.562 | 1.949 | 0.494 | 1.865 | 0.528 |
| 1.05 | 0.33 | 1.0 | 0.989 | 52% | 0.399 | 0.703 | 1.866 | 0.442 | 1.885 | 0.641 |
| 1.05 | 0.33 | 0.8 | 1.006 | 51% | 0.404 | 0.674 | 1.940 | 0.325 | 1.955 | 0.738 |
| 0.85 | 0.28 | 0.8 | 1.013 | 54% | 0.289 | 0.516 | 2.232 | 0.885 | 1.739 | 0.414 |
| 0.95 | 0.40 | 0.8 | 1.015 | 49% | 0.506 | 0.509 | 2.029 | 0.404 | 2.004 | 0.636 |
| 1.05 | 0.40 | 1.2 | 1.134 | 48% | 0.558 | 0.652 | 2.191 | 0.858 | 1.817 | 0.727 |
| 1.05 | 0.40 | 1.0 | 1.147 | 48% | 0.548 | 0.677 | 2.215 | 0.705 | 1.952 | 0.783 |
| 1.05 | 0.40 | 0.8 | 1.175 | 46% | 0.555 | 0.683 | 2.288 | 0.486 | 2.194 | 0.845 |

**Rule and decision.** Adopt the lowest-regret config only if it beats the old defaults by at least 0.03 BB overall and no pot-type or hero-role group gets worse by more than 0.1 BB. {0.85, 0.28, 1.2} is 0.124 BB better overall (0.867 → 0.743) and better in all six of those groups, so it is now Baseline's default (`ArchetypeParams` defaults in `range_model.h`). The Nit, Station and Maniac rows set all three fields explicitly and are unchanged. `kReferenceValueTop` (0.33) and the standard continue share `Cref` (0.95 × MDF), which the value thresholds use, are unchanged too, so Baseline's value threshold now shifts by a = −0.05.
- **Outside the rule:** wet boards get 0.111 BB worse (0.271 → 0.382), and the worst single spot, sb_btn_3bet-monotone-xx, goes from 5.9 to 6.8 BB (all-in 37% where the solver calls 92%).
- **Overfitting caveat:** the bench ranges are hand-written assumptions, and the winner sits at a corner of the grid on all three axes (lowest continue, lowest value_bet_top, highest bluff_mult), so the bench would probably prefer going further. It was not extended (one round only). Stage 8's turn spots are the planned holdout.
- **Direction:** the bench rewards a Baseline that folds more to bets (0.85 × MDF), value-bets a little tighter and bluffs more (a more polarised bettor). bluff_mult 1.2 beats 1.0 in 8 of the 9 (continue_mult, value_bet_top) pairs, all but (0.85, 0.40).

**Hold-out checks** (Stage 4 range-plausibility tests, not part of the bench; old defaults → new):

| Check | Bound in the test | Old | New |
|---|---|---|---|
| AQ vs BB flop check-raise (`CheckRaiseRangesAreStrong`) | 45-60% | 56.8% | 47.7% |
| AQ vs BB river check-raise, x/x turn | ≤ 40% | 27.5% | 31.1% |
| ... after calls on every street | ≤ 40% | 25.4% | 29.0% |
| AQ vs the BB's river lead (plausible about 55-65%) | < 80% | 72.8% | 67.5% |
| C-bet a third, 3-bet pot K83r (`CbetFrequenciesArePlausible`) | ≥ 85% | 89% | 89% |
| C-bet a third, BTN vs BB K72r | 60-80% | 71% | 66% |

`RangeModelReport.DISABLED_ThreeSpots`, Baseline rows only (the other types are identical):

| Spot | Old: mass, pair / air, folds to 33/75/125% | New |
|---|---|---|
| 1. BB's range, K72r c-bet/call, turn checked | 215.9, 45.7% / 51.0%, 28/46/58% | 207.5, 55.1% / 41.3%, 36/51/62% |
| 2a. BTN's river bet after bet-bet-bet | 66.3, 2 pair+ 18.4%, air 20.7%; Kc9c 26.8% | 45.9, 2 pair+ 25.1%, air 21.2%; Kc9c 27.6% |
| 2b. BB's check-call ×2, check | 100.9, pair 90.7%; AsQd 93.0% | 87.5, pair 91.7%; AsQd 93.4% |
| 3. CO calls a 3-bet, checks J83 | 51.1; AhKc 41.4%; folds 16/33/45% | 52.0; AhKc 41.3%; folds 25/38/49% |

- Every bound still holds, and every test passes unchanged. Check-raises are a little weaker (more bluffs at bluff_mult 1.2), and the flop check-raise is now near its lower bound (47.7% against 45%). The river lead moves toward the plausible 55-65%. Defending ranges are a little stronger (more folds), and fold shares to bets rise by 4-9 pp.

**The worst group: IP facing 75% in the 3-bet pot (diagnosis, nothing tuned or fixed).** On sb_btn_3bet-dry-xx (Kc8h3d2s6c, pot 665 including the SB's 285 lead, 1820 behind for the BTN, 1535 for the SB; pot odds 30%), with the old defaults:

| Combo | Range equity | Call EV | Best raise EV | Advice | Solver |
|---|---|---|---|---|---|
| JJ | 78.3% | 459.0 | 572.7 (to 998) | Raise to 998 | Call; regret 7.1 BB |
| KQo | 91.8% | 586.7 | 917.8 (to 1235); 898.6 (to 998) | Raise to 998 (size rule) | Call; regret 7.3 BB |
| AQo | 16.3% | −129.7 | −232.5 or less | Fold | Call; regret 5.7 BB |

- **The arithmetic is right.** Call = e × 950 − 285 (JJ: 0.783 × 950 − 285 = 459.0). JJ's raise to 998 = F·665 + P_call·(e_c·2376 − 998) + P_raise·(e_r·4020 − 1820), with F 45.4%, P_raise 8.6%, P_call 46.0%, e_c 57.8% and e_r 73.7%: 301.9 + 172.6 + 98.3 = 572.8. The re-raise is all in (1820), costing Hero 1820 and the SB 1535. KQo to 998: 314.5 + 435.8 + 148.8 = 899.1 (listed 898.6, from rounded shares).
- **Main cause: the inferred lead range, an assumption.** The model's SB range after it checked twice and led 75% is 4.6 combos: AQo 24%, 99 12.5%, TT 8.9%, A8s and A3s 8.4% each, AQs 8.1%, then KQo, AKo, QQ and AA at 2.5-3.8%. Kx, overpairs and sets are about 19%. The bench's range for the same lead is about 60% Kx, AA, KK, 88 and QQ and 40% missed draws (A5s, A4s, QJs, QTs, JTs, T9s and others), so, counted by hand from the spot's weights, JJ has about 37% there, not 78%, and AQ about 38%, not 16%. Two assumptions produce the model's range:
  - Checking keeps only slowplay × (1 − 0.15) = 17% of value per street, so a 3-bettor that checked flop and turn keeps about 3% of its value hands.
  - Value at the river lead is judged against the BTN's checked-back range (mostly AQ, AJ, AT and suited broadways), so 99 and TT count as value, and AQ bets as a medium hand (15%). AQ ties itself and loses to every pair, so its 16% equity follows.
  The bench's ranges tell the opposite story ("SB leads big with slowplayed Kx, overpairs and sets against missed wheel and straight draws"). Neither is solved. This is the Stage 4 weakness on river bets after passive play, not an EV bug, so it is logged, not tuned.
- **Second cause: the re-raise branch, against Hero's range before the raise.** The SB's re-raise range against JJ's raise has the same shape as its lead range (AQo 16.9%, 99 14.8%, TT 10.8% ...), because its value re-raises are judged against the BTN's capped checked-back range (decision 14), so e_r is 73.7% and the branch adds +98 chips. A test run narrowed Hero's range by `raise_likelihood` against the last aggressor, only when Hero raises. Then the re-raise range is mostly KK and sets plus a few weak-ace bluffs, the raise share falls from 8.6% to 2.2%, e_r falls to 27.3%, and JJ's raise falls to 528.3 (304.6 + 239.7 − 15.9). It still beats the call (459.0), on fold equity and the call branch. On the whole bench that test run gave 0.872 BB and 53% (old defaults; 0.867 and 54% without it), and 0.751 and 55% with the new defaults (0.743 and 56% without). IP vs 75% went from 1.719 to 1.730. 99 switched from raise to call, and the spot's regret fell from 3.9 to 3.4 BB, but other spots got worse. It is arguably more faithful to the range model's own rule ("for a raise the bettor's range includes its bet"), but it is no better on the bench, so it was not kept. Decision 14 stands, now measured for raises alone.
- **So:** the advice in this group follows from the model's belief that a 3-bettor's check-check-lead is medium-strength. Fixing that means changing slowplay and medium-bet assumptions in `src/range`, which is a range-model question for later stages with a holdout, not this tuning round.

**Final river bench** (new defaults; overall agrees with the grid row), against the Stage 3 baseline and batch A:

| Group | Stage 3 regret (BB) | Batch A | Now | Stage 3 agree | Batch A | Now |
|---|---|---|---|---|---|---|
| **Overall (36)** | **1.946** | **0.867** | **0.743** | **28%** | **54%** | **56%** |
| BTN vs BB | 0.939 | 0.324 | 0.288 | 27% | 59% | 61% |
| CO vs BTN | 1.422 | 0.530 | 0.435 | 26% | 53% | 52% |
| SB vs BTN 3-bet | 3.478 | 1.748 | 1.507 | 32% | 51% | 54% |
| Dry | 1.68 | 0.700 | 0.723 | 30% | 50% | 51% |
| Wet | 1.68 | 0.271 | 0.382 | 21% | 63% | 61% |
| Paired | 2.26 | 1.197 | 0.545 | 33% | 54% | 60% |
| Monotone | 2.17 | 1.300 | 1.322 | 30% | 50% | 51% |
| Check-check line | 1.38 | 1.118 | 1.094 | | 54% | 51% |
| Bet then check | 1.27 | 0.741 | 0.462 | | 49% | 55% |
| Bet-bet | 3.18 | 0.744 | 0.672 | | 59% | 61% |
| OOP first to act | 0.966 | 0.450 | 0.410 | 24% | 40% | 37% |
| IP facing 33% | 1.856 | 0.433 | 0.354 | 31% | 67% | 73% |
| IP facing 75% | 3.016 | 1.719 | 1.464 | 30% | 55% | 58% |

- Overall regret is 0.38× the Stage 3 baseline (target ≤ 0.75×), and agreement is +28 pp (target +10 pp). Regret as a share of the pot is 2.11% (5.38% at Stage 3, 2.52% at batch A). Regret above 1 BB covers 16% of hands (39% at Stage 3, 17% at batch A).
- **Worst spots now:** sb_btn_3bet-monotone-xx 6.8 BB, sb_btn_3bet-dry-xx 3.4, sb_btn_3bet-paired-bx 2.1 (was 4.9), sb_btn_3bet-paired-bb 1.7, btn_bb-monotone-bb 1.4, co_btn-wet-xx 1.3.
- **Advisor time per bench decision:** p50 1.99 ms and p95 2.60 ms (batch A: 2.25 and 3.23 ms, on a different container). Full run 12.5 s, 6.7 s of it solving.

**Hint latency** (`Advisor.DISABLED_LatencyByStreet`, Release, default options, ms, p50 / p95 / max, run alone):

| Spot | Lines off, auto threads | Lines on, auto threads | Lines off, 1 thread | Lines on, 1 thread |
|---|---|---|---|---|
| Preflop 3+ way (2010) | 2.6 / 3.3 / 7.2 | 5.6 / 7.7 / 66 | 4.2 / 5.3 / 18 | 12.6 / 17.1 / 50 |
| Flop 3+ way (1774) | 4.6 / 8.6 / 19 | 7.6 / 11.0 / 24 | 8.0 / 19.9 / 47 | 19.9 / 28.8 / 57 |
| Flop heads-up (300) | 3.8 / 4.2 / 5.0 | 4.8 / 6.5 / 10 | 11.2 / 16.6 / 22 | 13.8 / 14.7 / 17 |
| Turn 3+ way (1588) | 3.9 / 6.3 / 98 | 5.4 / 10.5 / 100 | 4.8 / 10.0 / 237 | 10.4 / 21.5 / 242 |
| Turn heads-up (300) | 1.5 / 1.7 / 2.2 | 2.1 / 2.8 / 8.6 | 1.5 / 1.9 / 2.5 | 2.6 / 2.9 / 6.9 |
| River 3+ way (1434) | 3.9 / 8.4 / 161 | 5.1 / 14.9 / 158 | 4.6 / 14.6 / 447 | 10.4 / 29.2 / 489 |
| River heads-up (300) | 1.3 / 1.4 / 1.8 | 1.8 / 2.2 / 7.3 | 1.3 / 1.6 / 2.1 | 2.8 / 3.0 / 8.6 |

- With automatic threads the targets hold: multiway flop worst case 24 ms (≤ 50), turn p95 10.5 ms and river p95 14.9 ms (≤ 15), heads-up turn and river p95 under 3 ms. On one thread river 3+ way p95 is 29.2 ms, as in batch B.
- The 3+ way maxima moved (turn about 100 ms, up from 33-42; river about 160 ms, down from about 255). The new parameters change which random-play ranges become nearly impossible to deal together, where the multiway redraw spikes come from (decision 17). The p95 values barely moved.

**Earlier numbers:** the batch A and B examples and tables above were measured with the old defaults; Baseline's shares in them (for example "Baseline 24%" folds in the 5c4d `why`) are now different. The archetype-line tests still pass.

**Tests:** all 428 pass in Release (5.7 s) and Debug (23.3 s), with no assertion changed. GCC Release/Debug and clang build with zero warnings, and `strict.sh` is clean on `training_advisor.cpp`, `range_model.cpp` and `advisor_bench.cpp`.

### Stage 5 review, uncommitted, on top of `0e388be`

**Fixed** (`training_advisor.cpp` `raise_value` and `evaluate_line`; formulas updated in `training_advisor.h`). All three were in multiway pots with unequal stacks only, so heads-up advice, the river bench (still 0.743 BB, 56%) and every existing test are unchanged.
- **The "everybody folds" term beside an all-in player.** It was F × P × k with k = Σ layer × e(all-in players) / Σ layer. The layers are the pot after Hero's call, so that left out the call Hero pays to play the main pot. It is now F × (Σ layer × e(all-in players) − to_call), which is F × P when nobody is all in (k = 1, bit-identical). Real numbers, BTN JhTh on 9h8c2h Kd, BB all in for 300, SB bets 500 into 900: the main layer is 900 (Hero has 36.8% against the BB's range) and the side layer 1000, so folds win 900 × 0.368 + 1000 − 500 = 831, not 1400 × 1331 / 1900 = 981. The all-in EV fell from 265.3 to 197.7, and the 25% and 50% raises from 207.1 / 256.1 to 166.7 / 194.5 (all-in is still the advice; the Maniac line flips from all-in to call). The bug predates Stage 5 (the old `raise_ev` had the same k).
- **A villain too short to cover the bet, raising.** The re-raise branch had the villain put in `to` − its street bet even when its stack was smaller. Now T and T' stop at the villain's stack (it can only call all in), and the part of Hero's bet it cannot match comes back.
- **A villain too short to cover the bet, calling.** V_called charged Hero the whole `a` against a short caller. It now adds refund = Σ w_i (1 − e_i) x_i, x_i being the chips caller i cannot match, so a short call is worth e_i (P + a_i + m_i) − a_i. Zero when everybody covers. Real numbers, BTN KsKd on Kh8c2h checked to three-way with the BB 240 behind: the all-in's EV moves from 398.8 (both short-stack errors) to 380.9 (BB call: 0.856 × 660 − 240 = 324.6 instead of 79; BB "raise": 338.2 instead of 1,617). The advice stays BET 135.
- `run_tasks` reserves its thread vector before starting threads, so a failed reallocation cannot destroy running threads.
- New test `Advisor.RaiseEvsCountWhatShortStacksCanMatch` rebuilds the documented formula per villain for both spots; it fails on `0e388be` (207.1 vs 166.7, 265.3 vs 197.7, 398.8 vs 380.9).

**Hand-checked spots** (scratch program, default options, one thread; every advisor EV matched the hand recomputation from the range model to 1e-3 chips):
1. *Heads-up flop c-bet:* BTN AhQd on Qc7s2d, pot 200, checked to. Range equity 88.0% (87.5% random); check 176.0. 50%: F 0.433, call 0.494, raise 0.073, e_call 0.872, implied capped at 200, V_called 448.9, V_raised 338.4, EV 333.0; 100% 364.1; all-in 277.9. Advice BET 100 (size rule; the model prefers 100%). Nit 50, Station 150. Sane.
2. *Multiway turn with a side pot:* the JhTh spot above. Call EV 131.4 against a 400,000-rollout layered check of 134.6 (layer equities 30.9% and 35.6%, so within Monte Carlo noise at 3,000 rollouts). Advice ALL-IN (1700 into 1400, within 1.5 pots) with a combo draw at 30% range equity: aggressive but defensible given 45% folds; the called branch (−346) is the model's single-pot simplification against an all-in player.
3. *Preflop 3-bet:* BTN vs a CO open to 50, blinds behind. AJs: model raise 45.5 − prior 40.75 = 4.7, call −5.2 + 32.5 = 27.0, so CALL (chart call). KTo FOLD (raise −1.5). 72o FOLD (raise model +26.6 before the −40.75 prior). QQ RAISE to 180 (model 118.5 + 20). Each villain's answer recomputed from `predict_preflop_response` (levels 2, size ratio to / 150).

**Found, not fixed (larger or by design):**
- *Players yet to act count as certain callers with their full ranges.* Call and check EVs use equity against every Active player, including those who have not acted this street; preflop that means untouched blind ranges. BTN AJs facing a CO open is 34.3% "against the range" (three-way) instead of about 45% heads-up, and CO AQo facing a 10 BB UTG shove with three players behind is 27.8% and FOLD (call −10.3 after a +70 chart prior), though calling is clearly profitable heads-up. The pre-Stage-5 advisor folded there too (random-hand equity against 4). Suggest weighting players behind by their continue share (Stage 6).
- *Preflop raises below the standard size gain no calls.* `predict_preflop_response` widens the call chart for size_ratio < 1, but `resize_range` grows it in strength order through classes already in the raise chart, so call shares are flat from ratio 0.4 to 1.0 (CO vs a 3-bet: 5.70% call at every ratio from 0.55 to 1.01). A min-3-bet then folds out as much as a standard one, which flatters small raises with weak hands (AJs: model 45.5 at 83 vs 33.7 at 180). Range model (Stage 4) issue, for the Stage 6 sizing review.
- *A size-rule bet that takes the stack becomes all-in even beyond 1.5 pots* (pre-existing). Facing a bet, a 75% or 100% raise that does not fit the stack is listed only as all-in, and the fit chooses it without the 1.5-pot test. The all-in is then smaller than the raise the rule chose, so it is arguably not an over-shove; left as is.
- *Latency target:* the plan's ≤ 10 ms on the turn and river holds heads-up (p95 under 3 ms) but not multiway (re-measured: turn p95 11.1, river p95 15.1 ms, automatic threads). The ≤ 15 ms figure in batch B is not the plan's.

**ThreadSanitizer:** clang's TSan runtime is not installed here, so GCC 13 `-fsanitize=thread` (RelWithDebInfo, `/tmp/claude-0/b/tsan`) ran `Advisor*:HandVsRange*:MultiwayRangeEquity*` (51 tests) before the fixes, and the same plus `Controller*` (78 tests) after them: all passed, with no ThreadSanitizer reports. Default options use automatic threads, so the parallel lines and the parallel equity code both ran.

**Also checked:** no `Seat`, `.hole`, engine headers, `showdown_ranks` or `last_result` in `src/advisor` or `src/range`, and the advisor reads only `PlayerView`; the logger and controller are unchanged since `e531106`, mistakes use `base_ev` with the 1 BB gap, and a seeded `LocalGameController` session (40 hands) wrote exactly the documented key set; `ThreadCountDoesNotChangeTheHint` covers serial against parallel lines on a multiway flop; the only shared statics are function-local `const` tables; Debug suite 23.2 s.

**Tests:** 429 pass in Release (5.8 s), Debug (23.2 s) and clang, with zero warnings; `strict.sh` clean on `training_advisor.cpp`, `range_model.cpp` and `advisor_bench.cpp`.

### Stage 6 batches A and B (positional 3-bet/4-bet charts, push/fold tables), uncommitted, on top of `cf3b6c3`

**Batch A: positional answers to 3-bets and 4-bets** (`preflop_charts.{h,cpp}`)
- New: `vs_three_bet_ranges(opener, three_bettor, players)` and `vs_four_bet_ranges(three_bettor, four_bettor, players)`. The bots' `vs_three_bet_response` and `vs_four_bet_response` are untouched, and `VsReraises.BotsChartsUnchanged` pins their answers for all 169 classes.
- Assumed spot: 100 BB 6-max, open to 2.5 BB, 3-bet to about 3x in position and 3.5-4x from the blinds, 4-bet to about 22-25 BB.
- "In position" is by postflop order (SB, BB, UTG, HJ, CO, BTN). An opener facing a later non-blind 3-bettor is out of position; facing a blind 3-bettor it is in position. Heads-up (players == 2) uses its own table. An SB or BB "opener" (a raise over limps) uses the SB-vs-BB table.
- The 4-bet ranges are value plus A5s/A4s-type blocker bluffs. Every continue hand is in the opener's own RFI range (tested for every opener/3-bettor pair).

Continue share = (4-bet + call combos) / the opener's RFI combos:

| Table | Used for | Intended | Measured (4-bet + call) |
|---|---|---|---|
| UTG, out of position | UTG vs HJ/CO/BTN | 38-45% | 42.9% (24 + 66 of 210) |
| UTG, in position | UTG vs SB/BB | 46-55% | 50.5% (24 + 82) |
| HJ, out of position | HJ vs CO/BTN | 36-44% | 40.7% (24 + 86 of 270) |
| HJ, in position | HJ vs SB/BB | 44-53% | 48.9% (24 + 108) |
| CO, out of position | CO vs BTN | 30-40% | 35.7% (42 + 90 of 370) |
| CO, in position | CO vs SB/BB | 40-50% | 45.4% (42 + 126) |
| BTN | BTN vs SB/BB | 40-55% | 48.9% (50 + 248 of 610) |
| SB | SB vs BB | 30-40% | 38.2% (56 + 160 of 566) |
| Heads-up BTN | BTN vs BB, 2 players | 45-60% | 52.9% (86 + 440 of 994) |

- For comparison, the old non-positional chart (106 combos) keeps 50.5% of a UTG open, 39.3% of HJ, **28.6% of CO**, 17.4% of BTN, 18.7% of SB and 10.7% heads-up. The CO-vs-BB entry is the must-fix's "CO folds 72%" (Stage 5 batch B); the new table folds 54.6%. Batch C has to switch the advisor and range model to these tables before the BB 3-bet share can be re-measured.
- Earlier openers keep a larger share of their opens (UTG > HJ > CO, both in and out of position). The BTN keeps 48.9%, more than the CO's 45.4%, because the blinds 3-bet it wider.

Answers to a 4-bet, as a share of all 1,326 combos:

| Table | Used for | 5-bet jam | Call | Continue |
|---|---|---|---|---|
| In position vs early | non-blind 3-bettor vs a UTG/HJ 4-bet | KK+, AKs | QQ, JJ, AKo, AQs | 3.3% |
| In position | non-blind 3-bettor vs a later 4-bet | KK+, AKs | QQ-TT, AKo, AQs, AJs, KQs | 4.4% |
| Out of position | blind 3-bettor vs a non-blind 4-bet | QQ+, AK | JJ, TT, AQs | 3.8% |
| Blind vs blind | both blinds | QQ+, AK | JJ-88, AQs-ATs, KQs, KJs, AQo | 6.8% |
| Heads-up | 2 players | QQ+, AK | JJ-66, AQs-A9s, KQs, KJs, KTs, QJs, JTs, AQo, AJo, KQo | 10.7% |

**Batch B: push/fold tables** (`tools/gen_push_fold.cpp`, `src/strategy/push_fold.{h,cpp}`, `src/strategy/generated/push_fold_data.cpp`)
- API: `push_fold_response(hand, hero, stack_bb, shover, players)`. First in it gives Raise (all in) or Fold; facing a lone jam, Call or Fold. It gives nullopt outside 3-15 BB, for the BB first in, and for a hero who doesn't act after the shover. `push_fold_jam_range` and `push_fold_call_range` return the `HandRange` for the range model, and `push_fold_iterations()` returns 2000.
- **Method:** fictitious play over the 169 classes, one game per first-in position and stack (3-15 BB in 1 BB steps, blinds 0.5/1, no ante, every player holding the stack). Strategies start as "jam everything, call half". Each iteration the jammer best-responds to the callers' averages, its average moves 1/(t+1) toward that, and then each caller best-responds to the jammer's updated average (alternating updates). The published set is every class whose average is at least 0.5.
- **Inputs:** `class_vs_class_equity` (200,000 rollouts per pair). Card removal between the jammer and each caller uses combo counts: P(j | i) = disjoint combo pairs (i, j) / (combos(i) × 1225). That is exact at class level for two players.
- **The one-caller approximation** (a known approximation for the decision log and CLAUDE.md section 12):
  - For UTG-BTN first in, every player behind is a possible caller, but the first one to call is the only one: overcalls are ignored.
  - The jammer's EV is Σ_m P(all before m fold) × (its showdown against m's calling range) + P(all fold) × 1.5 BB.
  - Each caller's range is its heads-up best response to the jam, as if it were the only caller. Players behind it don't change its range.
  - Card removal between callers, and from folded players, is ignored. The callers' fold probabilities are treated as independent.
- **SB vs BB is the heads-up game,** so heads-up the button, which posts the small blind, uses the SB tables. A short-handed table drops early positions, so each position keeps the same number of players behind and uses the same tables.
- **Determinism:** no randomness, fixed summation order, and strict "better than folding" tests. The output is byte-identical with 1 and 4 threads.
- **Runtime:** 65 games × 2,000 iterations in 2.9-3.1 s on 4 threads (11.7 s on 1). Ten thousand iterations take 14.8 s.
- **Convergence at 2,000 iterations:**
  - NashConv is the sum over players of what a best response gains, each caller's term counted conditional on facing the jam. For the published (pure) sets it is at most 5.9 mBB per hand (UTG at 3 BB), and at most 0.32 mBB for every SB/heads-up game.
  - For the mixed averages it is up to 57.6 mBB (UTG at 15 BB). That is mostly the 1/t tail of FP's early iterations, since the thresholded sets do better. At 10,000 iterations the figures are 10.8 mBB (averages) and 4.4 mBB (sets).
  - The sets are not frozen. The last flip of any set was at iteration 1,989, from classes sitting at the indifference point.
  - Against 10,000 iterations, the 2,000-iteration sets differ by 0.44% of combos per set on average. The worst cases are the multiway 3 BB games: SB calling a UTG jam (64 combos, 4.8%), and the UTG 3 BB jam (60 combos). Apart from 3 BB, the largest difference is 36 combos (BB vs a UTG jam at 6 BB).
- **Heads-up at 10 BB: the SB jams 58.37% and the BB calls 37.25%,** against the commonly published 58% and 37%.
  - At 5 BB: jams 71.3%, calls 62.0%. At 15 BB: jams 45.4%, calls 28.8%.
  - `GeneratedTableMatchesARecomputation` rebuilds the heads-up model independently at 5, 10 and 15 BB and checks each published set against a best response to the other (allowing up to 3 classes per side within 0.05 BB of indifference). Measured: the jam sets differ in 0, 0 and 1 class (0.007 BB from indifference), and the call sets in 1 class at each stack (0.003, 0.005 and 0.029 BB).

Jam share, as a percent of all combos:

| Position | 5 BB | 10 BB | 15 BB |
|---|---|---|---|
| UTG | 25.2 | 16.0 | 10.1 |
| HJ | 29.1 | 19.8 | 13.7 |
| CO | 32.4 | 24.3 | 18.3 |
| BTN | 40.9 | 32.1 | 26.4 |
| SB | 71.3 | 58.4 | 45.4 |

- **BB call share** against a jam at 5 / 10 / 15 BB: vs UTG 31.8 / 9.7 / 5.6; vs CO 38.8 / 17.2 / 10.0; vs BTN 45.7 / 24.3 / 16.1; vs SB 62.0 / 37.3 / 28.8. Non-blind callers share one range per jammer, since they face the same price.
- **Monotonicity:** every jam and call share is non-increasing in the stack from 3 to 15 BB. A few adjacent stacks have equal shares (jams: UTG 13-14, CO 11-12 and 13-14, BTN 10-11), but no share rises. Earlier positions jam strictly tighter at every stack.
- **Call sets are equity thresholds:** in every call set, the weakest called class has at most 0.94 pp less equity against the published jam set than the best folded class. That gap exists because the sets threshold mixed averages.
- **One ordering exception, by design:** at 3 BB the BB calls a BTN jam wider (97.3%) than an SB jam (92.8%), because the SB's dead half blind improves its price. `EarlierPositionsJamTighter` checks "later jams are called wider" from 4 BB.

**Tests:** 440 pass in Release (5.8 s) and Debug (24.0 s), with zero warnings in Release, Debug and clang. `strict.sh` is clean on `preflop_charts.cpp`, `push_fold.cpp`, `gen_push_fold.cpp`, the generated data and both test files. Every new test takes at most 0.02 s in Release.

### Stage 6 batch C (advisor and range model on the new tables), uncommitted, on top of `d623daf`

**What it adds:** `src/advisor/advisor_charts.{h,cpp}` (`advisor_chart`, `advisor_chart_response`), `PreflopSpot` and its `predict_preflop_response` overload in `range_model.h`, and the players-still-to-act call model in `training_advisor.cpp`. The bots, the JSONL schema, mistakes on `base_ev` and the snapshot are unchanged.

**Range model**
- *Positional 3-bets and 4-bets:* the opener facing a 3-bet now answers from `vs_three_bet_ranges(opener, 3-bettor)`, and the 3-bettor facing a 4-bet from `vs_four_bet_ranges(3-bettor, 4-bettor)`, in inference and in `predict_preflop_response` (`villain_raised_last`). Everyone else (a cold caller squeezed, a player yet to act, a cold 4-bet) keeps the non-positional tables. The walk now records who made each raise level.
- *Short all-ins:* the temporary formulas (top 400/BB %, halved facing a raise; calls the top 250/BB %) are gone. A committing raise (all in, or at least 40% of the starting stack) of depth D BB uses the push/fold jam table first in, the call range against a jam from the last raiser when it is a re-jam, and a call of an all-in uses the call range against the jammer. Callers who acted before the jammer use the BB's range against it. From 15 to 25 BB the likelihood is (1 - t) × table + t × commit range, t = (D - 15) / 10. For a BTN jam first in (Baseline, combos, before card removal): 15 BB 350 (the 26.4% table), 16 BB 319, 17 BB 288, 20 BB 195, 25 BB 40 (the commit range JJ+, AK); the test checks every class of the blend. The old model went from 352 (the top 26.7%) at 15 BB to 40 at 16 BB.
- *Calls below the standard size (must-fix D):* the cause was `resize_range` growing the call chart in strength order through hands already in the raise chart. Below the standard size the range now gains (f - 1) × its call mass from its own strongest folding classes, f = ratio^-1; above it nothing changed (f = ratio^-0.5). Measured, Baseline:

| Spot | Size (× standard) | Calls before | Calls now |
|---|---|---|---|
| BB vs a BTN open of 1.5 / 2 / 2.5 BB | 0.6 / 0.8 / 1.0 | 38.7 / 38.7 / 38.7% | 64.9 / 48.4 / 38.7% |
| CO open vs a BB 3-bet (positional table) | 0.53 / 1.0 / 1.4 | flat below 1.0 | 68.2 / 36.3 / 30.9% |

  The 3-bets are unchanged by size. The Stage 5 review's CO-vs-3-bet example (5.70% calls at every ratio from 0.55 to 1.01) used the non-positional chart against a uniform range; the same flatness is what the new test rules out.

**Must-fix A: the BB 3-bet everything against a CO open** (100 BB, open to 2.5 BB, Baseline, lines off, 169 classes weighted by combos)

| Spot | Before (`d623daf`) | After |
|---|---|---|
| BB vs CO: 3-bet / call / fold | 76.3 / 23.7 / 0% (72o: RAISE to 80) | 4.5 / 23.7 / 71.8% (72o: FOLD) |
| BTN vs CO | 7.5 / 8.6 / 83.9% | 5.7 / 8.6 / 85.7% |
| SB vs BTN | 100 / 0 / 0% | 13.7 / 0 / 86.3% |

- After: BB 3-bets AA KK QQ JJ AKs AKo AQs KQs A5s A4s A3s, to 133 or 160 (2.7-3.2×); BTN AA KK QQ JJ AKs AKo AQs KQs AQo KJs A5s A4s A3s, to 115-180; SB the chart's 30 classes (88+, A8s+, A5s-A2s, ATo+, KTs+, KJo+, QTs+, JTs, T9s), to 140 or 170. Each equals its chart's 3-bet range: the model no longer outvotes the prior here.
- Cause: the non-positional vs-3-bet chart (the CO kept 28.6% of its open) and, for the min-3-bets, `implied` (below). With the positional table the CO continues 45.4% against a standard BB 3-bet; 72o's model EV at 160 is +11.5 chips (F 55.2%: 44.2; called 35.4% at 25.8% equity: 0.354 × (0.258 × 330 - 140) = -19.4; re-raised 9.4%: -13.2), against a 55-chip rejection prior (was +50 against 35). The raw model still slightly favours 3-betting junk; the prior decides.
- The share is below the 8-15% band (see Deviations).

**Must-fix B: players still to act** (Baseline, default options)

| Spot | Before | After | Heads-up | All still in |
|---|---|---|---|---|
| CO AQo vs a 10 BB UTG jam, BTN and blinds behind | 27.8%, FOLD (call -10.3 after a +70 prior) | 48.4%, CALL 200 (model +18.1, ev 88.1) | 53.8% | 26.3% |
| BTN AJs vs a CO open, blinds behind | 34.6%, CALL (27.5) | 52.4%, CALL 50 (53.5) | 58.3% | 34.9% |
| UTG AQs facing a half-pot flop bet from the BB, CO and BTN behind | | 18.4%, FOLD | | 13.6% |

- Hand check of the AQo call (approximate): heads-up 0.538 × 430 - 200 = +31.3. The BTN, SB and BB call a 10 BB UTG jam with their push/fold call ranges (8.0, 8.3 and 9.7% of hands), so nobody overcalls 76.2% of the time and exactly one does 21.7%. Three-way AQo has 29.5% (with the BB) or 28.1% (with the BTN) by a 200,000-rollout check, worth about 0.29 × 620 - 200 = -20. 0.762 × 31.3 + 0.217 × (-20) + about 2% × (-60) ≈ 18.4, against the model's 18.1.
- The squeeze branch: Hero calls a pot-sized squeeze when the equity against the squeeze range pays, otherwise loses the call (`V_sq`). The archetype lines use the same model with their own ranges and parameters (tested in the AJs spot: every line's range equity is more than 5 pp above its equity against all three players).
- Heads-up spots are unchanged, so the river bench moves only through the preflop ranges (below).

**Must-fix C: open sizes** (combos per size first in, 100 BB; 40 = 2 BB, 45 = 2.25, 58 = 2.9, 70 = 3.5; the SB's 40 / 50 / 60 = 2 / 2.5 / 3 BB)

| Position | Before (`d623daf`) | Must-fix D only | After (D and `implied` 0 preflop) |
|---|---|---|---|
| UTG (210) | 40:154 45:10 70:46 | 40:116 45:48 70:46 | 40:106 45:64 58:6 70:34 |
| HJ (270) | 40:214 58:10 70:46 | 40:122 45:96 58:6 70:46 | 40:112 45:112 58:12 70:34 |
| CO (370) | 40:276 45:4 58:18 70:72 | 40:176 45:122 70:72 | 40:164 45:130 58:16 70:60 |
| BTN (610) | 40:448 45:22 58:26 70:114 | 40:188 45:274 58:34 70:114 | 40:32 45:404 58:66 70:108 |
| SB (566) | 40:322 50:54 60:190 | 40:184 50:186 60:196 | 50:252 60:314 |

- Cause, with numbers: first, calls did not grow below the standard size (must-fix D), so the minimum raise bought the same fold equity more cheaply. Second, `implied` = min((e_call - 0.5)+ × behind × 0.5, 0.5 × pot): at 100 BB each point of preflop all-in equity against the calling range was worth about 10 chips, so the size whose callers were weakest won. BB AJs vs the CO: implied 85 / 78 / 42 / 32 chips at 80 / 105 / 133 / 160, which made a min-3-bet the advice (model 100.7 against 87.6 at 105); for strong hands the cap (half the pot) grows with the raise, which pushed them to the largest size. `implied` is now 0 preflop; the open ranges and every all-in-or-fold test are unchanged.
- After: typical opens are 2-2.25 BB (2.25 from the BTN), with 3.5 BB for AA KK QQ AKs AKo from UTG and HJ, plus JJ, AQs, AJs and AQo from the CO, and the strongest 16 classes from the BTN. That remaining size tell is the model's (a fixed answer per size, with calls shrinking only by ratio^-0.5 above the standard size, rewards strong hands for building a pot); not fixed. SB opens 2.5-3 BB, as it should.

**Push/fold in the advice** (Baseline, lines off; the tests use fast options at 10 BB): at 10 BB first in from UTG, CO, BTN and SB, every class jams or folds exactly as the table says (169/169 each, also at 5 and 15 BB); the BB facing a lone 10 BB jam from the SB, BTN or CO calls or folds exactly per the table (169/169 each, also at 5 and 15 BB). At 16 BB the table no longer applies.

**Chart-agreement oracle:** `FacingAShoveAgreesWithTheChartEverywhere`, `FacingANormalOpenFollowsTheChartOnWhetherToPlay` and the `chart_response_for_shove` checks now take `advisor_chart_response` as the oracle. In those 100 BB single-raise spots it equals `chart_response` for all 169 classes (`AdvisorChartMatchesTheBotsChartOutsideItsOwnTables`).

**River bench** (`advisor_bench`, defaults): overall **0.708 BB, 56% agree** (Stage 5: 0.743, 56%). Only the 12 SB-vs-BTN 3-bet spots moved, because the BTN's flat of the SB's 3-bet now comes from the positional table (`btn_vs_three_bet`, wider): SB vs BTN 3-bet 1.507 → 1.402 BB (54% → 53%); BTN vs BB 0.288 and CO vs BTN 0.435 unchanged. Spot changes: dry-xx 3.446 → 2.457, dry-bx 0.624 → 0.447, paired-xx 0.186 → 1.561, paired-bx 2.064 → 1.163, paired-bb 1.719 → 1.382, monotone-xx 6.818 → 6.148, monotone-bx 0.446 → 0.262, monotone-bb 1.166 → 1.818, wet-xx 0.216 → 0.245, wet-bx 0.549 → 0.498. Groups: dry 0.594, wet 0.379, paired 0.560, monotone 1.299; OOP first 0.354, IP vs 33% 0.519, IP vs 75% 1.251. The bench's ranges are fixed, but the advisor reads the opponents' ranges through the walk, so a different preflop flat range changes its river advice. Not tuned.

**Hint latency** (`Advisor.DISABLED_LatencyByStreet`, Release, default options, ms, p50 / p95 / max, run alone)

| Spot | Lines off, auto threads | Lines on, auto threads | Lines off, 1 thread | Lines on, 1 thread |
|---|---|---|---|---|
| Preflop 3+ way (2010) | 2.8 / 3.7 / 8.8 | 5.9 / 8.2 / 19 | 4.2 / 5.3 / 17 | 13.0 / 17.9 / 49 |
| Flop 3+ way (1774) | 4.8 / 8.4 / 14 | 8.0 / 11.3 / 23 | 8.0 / 19.8 / 47 | 20.0 / 27.6 / 43 |
| Flop heads-up (300) | 3.7 / 4.5 / 5.6 | 4.7 / 5.7 / 8.1 | 11.1 / 12.2 / 21 | 14.1 / 14.9 / 20 |
| Turn 3+ way (1588) | 4.0 / 6.1 / 106 | 5.7 / 9.0 / 107 | 4.7 / 8.1 / 239 | 10.7 / 18.1 / 246 |
| Turn heads-up (300) | 1.5 / 2.3 / 5.0 | 2.1 / 2.6 / 5.5 | 1.5 / 1.6 / 3.1 | 2.7 / 2.8 / 6.2 |
| River 3+ way (1434) | 3.9 / 6.9 / 79 | 5.3 / 9.7 / 92 | 4.3 / 8.5 / 211 | 10.6 / 20.0 / 214 |
| River heads-up (300) | 1.3 / 1.9 / 4.0 | 1.7 / 2.0 / 5.8 | 1.2 / 1.4 / 2.1 | 2.9 / 3.1 / 5.3 |

- With automatic threads every 3+ way p95 is at most 11.3 ms (target 15): preflop 8.2 (Stage 5: 7.7), flop 11.3 (11.0), turn 9.0 (10.5), river 9.7 (14.9).
- A first version (the continuing ranges' Monte Carlo at full rollouts) measured turn p95 17.9 and river 17.3 ms (lines on, 20 games). Callgrind put 24% of all time in that Monte Carlo, mostly redrawing deals of narrow call ranges that collide. Two changes fixed it: when the call model applies, the equity against everyone still in is no longer computed (nothing uses it then), and the continuing Monte Carlo draws at most about 30,000 deals (a 512-deal acceptance trial sets the rollouts, at least 100). The river and turn are now faster than at Stage 5 because the skipped Monte Carlo was where some of the redraw spikes were.

**Tests:** 447 pass in Release (6.6 s) and Debug (26.7 s), 440 + 7 new. GCC Release/Debug and clang build with zero warnings; `strict.sh` is clean on `advisor_charts.cpp`, `training_advisor.cpp` and `range_model.cpp`. Each new test takes at most 0.7 s in Release (1.9 s in Debug).

### Stage 6 review, uncommitted, on top of `becbe81`

No defect needed a code change; one test comment was moved. Bench unchanged (0.708 BB, 56%). 447 tests pass in Release and Debug, zero warnings in Release, Debug and clang, `strict.sh` clean on the six Stage 6 files and the generated data.

**Verified**
- Push/fold solver: the jammer's EV (sum over callers of P(all before fold) × (equity × pot - stack) + P(all fold) × (1.5 - own blind)), the caller's gain (equity × pot - stack + own blind), the pots (2S + 1.5 - both blinds) and the combo-count card removal are right. "All fold" is the product of the callers' independent fold shares, as documented. `gen_push_fold` regenerates the committed data byte for byte. Lookup: heads-up the BTN reads the SB tables, and the engine names heads-up seats BTN and BB, so the mapping holds.
- An independent best-response check of the multiway sets (UTG-BTN at 4, 8, 10, 12 and 15 BB) against each other: 0-9 jam classes and 1-15 call classes per game sit on the wrong side of indifference, by at most 0.40 BB per hand of that class (CO and BTN folding KQs to a 12 BB HJ jam; BB folding A9s to a 10 BB UTG jam, +0.25). These are classes the averages mix; thresholding at 0.5 makes the pure sets slightly off each other. About 1 mBB per hand dealt, consistent with the NashConv figures. Not changed.
- Charts: every positional continue hand is in the opener's RFI (tested), and `BotsChartsUnchanged` is a literal snapshot; no bot file changed.
- Players still to act, two hand checks with independent Monte Carlo (400,000 and 300,000 rollouts, each scenario dealing only its own players):
  - CO AcQd facing a 10 BB UTG jam, BTN/SB/BB behind: call shares 6.86 / 7.10 / 8.33% (Hero's blockers removed; the model's are identical), P(nobody) 0.793, heads-up 53.97% exact (the class matrix says 53.80%), three-way 29.4-30.9%. Independent call EV +22.9 and 48.9% range equity; the model gives +18.1 and 48.2%. Dealing the folded players from their call ranges, as the model does, gives +19.4 (three-way equity falls about 2.3 pp per scenario); the matrix and the blinds' smaller calls account for most of the rest. CALL either way.
  - UTG AcQc on 2c7dJh facing the BB's 100 into 210, CO and BTN behind: each folds 42.3%, calls 49.6%, raises 8.1%; Hero folds to a squeeze (V_sq = -100). Independent call EV -17.7 and 19.5%; the model -21.7 and 18.7%. FOLD either way.
  - So the "folded players hold call-range cards" approximation (listed under Unverified) biases multiway call EVs down by about 3.5-4 chips (0.2 BB) in both spots. A fix needs per-scenario dealing or fold-range weighting inside `equity_vs_ranges`; not done.
- `implied` is used only in `raise_value`; nothing else read it preflop.
- No hidden information or engine types in `src/advisor`, `src/range`, `src/strategy` or the tool; the new static tables are function-local statics (thread-safe initialisation); the continuing Monte Carlo is seeded from the line seed.

**Sanity tables** (Baseline, lines off, all 169 classes, combos of 1326)

| Spot | Advice |
|---|---|
| 100 BB first in | UTG opens 15.8% (2 BB 8.0, 2.25 BB 4.8, 2.9 BB 0.5, 3.5 BB 2.6); HJ 20.4%; CO 27.9%; BTN 46.0% (mostly 2.25 BB); SB 42.7% (2.5-3 BB). Every class agrees with `advisor_chart`. |
| 12 BB first in | jam or fold only: UTG 13.4%, HJ 17.2%, CO 22.5%, BTN 31.2%, SB 53.2%, exactly the tables. |
| BB vs a BTN 2.5 BB open | 3-bet 6.5% (to 8 BB), call 37.4%, fold 56.1%, all per the chart. |

Judgement: the open ranges and the 12 BB jams are in line with common 6-max charts and push/fold tables. The BB's defence against the BTN is tight next to solver outputs (which defend about 55-65% with 12-15% 3-bets); that is the bots' BB-vs-BTN chart, which this stage may not change. The open sizes are plausible in size, but the 3.5 BB opens with premiums are a size tell (already noted under batch C).

**Found, not fixed**
- `advisor_charts.cpp` `push_fold_answer`: facing a lone jam, the table applies whenever Hero's effective stack is at most 15 BB, even when the jammer is deep and a deep player was still to act behind it. Example: BB with 10 BB, SB 100 BB, BTN jams 100 BB. The range model reads that jam at 100 BB depth (JJ+, AK), but the chart prior is the BB's 10 BB call range against a BTN jam, so 22 and 55 are advised to call (model -50 chips, prior +65). Restricting push/fold to jams the range model also reads as short (tried) does not change the advice: the jam is a call of 9 BB, not "committing" by the bots' test, so the chart falls back to `vs_open_response`, which also calls 22. A real fix means a commit-aware answer to deep jams in the chart; left for a decision.
- The inference (a villain's range after it calls) ignores raise size, while `predict_preflop_response` widens calls below the standard size: a flat of a min-raise is read as the standard flat range. Pre-existing for sizes above standard; not changed.

**Test changes:** none to assertions. The `DISABLED_LatencyByStreet` comment, which batch C's new tests had separated from its test, is back above it.

### Stage 6 follow-ups, uncommitted, on top of `25faff8`

Five fixes from the Stage 6 review. The bots, the JSONL schema, the snapshot and mistakes on `base_ev` are unchanged; `VsReraises.BotsChartsUnchanged` now also pins the bots' answers to an open for every hero, opener and table size.

**1. Advisor-only answers to an open** (`advisor_vs_open_ranges` in `preflop_charts.{h,cpp}`)
- The bots' `vs_open_ranges` stays for the bots. The advisor's chart (`advisor_chart`, one raise and no commit) and the range model (inference and prediction of 3-bets and flats facing one raise, and a SB flat, read as the BTN's call range against that opener) use the new tables.
- Authored at 100 BB, open to about 2.5 BB, in the style of published 6-max solver outputs (not solved, not compared with a specific one). The BB tables are written as the 3-bets plus everything defended, the call range being the difference, so nesting is easy to see. Every 3-bet range has value plus a polarized share: suited wheel aces in all of them, suited connectors (54s-T9s for the BB, 65s/54s/76s/87s for the BTN and CO, 87s/98s for the SB) and suited broadways. AJs and AJo flat on the button (see the min-3-bet finding below).
- One per hero and opener: HJ, CO and BTN against each earlier opener (a non-blind hero against a later or blind opener reads BTN vs CO), the SB against UTG, HJ, CO and BTN or later, the BB against each opener and heads-up. Each 3-bet range and each defended range contains the one against an earlier opener, and against an earlier hero for the same opener (tested).

Chart shares, % of all 1326 combos (bots' table before, advisor's table now; the advice equals the chart in all 169 classes of every row, measured with the Baseline advisor, lines off, open to 50):

| Hero vs opener | Bots: 3-bet / call / defend | Advisor: 3-bet / call / defend |
|---|---|---|
| HJ vs UTG | 3.2 / 6.3 / 9.5 | 5.1 / 3.3 / 8.4 |
| CO vs UTG | 3.2 / 6.3 / 9.5 | 5.4 / 4.1 / 9.5 |
| BTN vs UTG | 3.2 / 6.3 / 9.5 | 5.4 / 7.1 / 12.5 |
| CO vs HJ | 3.2 / 6.3 / 9.5 | 6.3 / 4.4 / 10.7 |
| BTN vs HJ | 3.2 / 6.3 / 9.5 | 7.8 / 8.6 / 16.4 |
| BTN vs CO | 5.7 / 8.6 / 14.3 | 9.5 / 9.4 / 18.9 |
| SB vs UTG | 4.2 / 0 / 4.2 | 6.5 / 0 / 6.5 |
| SB vs HJ | 4.2 / 0 / 4.2 | 8.1 / 0 / 8.1 |
| SB vs CO | 7.7 / 0 / 7.7 | 10.7 / 0 / 10.7 |
| SB vs BTN | 13.7 / 0 / 13.7 | 14.2 / 0 / 14.2 |
| BB vs UTG | 2.9 / 16.9 / 19.8 | 8.1 / 24.0 / 32.1 |
| BB vs HJ | 2.9 / 16.9 / 19.8 | 9.5 / 28.7 / 38.2 |
| BB vs CO | 4.5 / 23.7 / 28.2 | 11.5 / 37.6 / 49.0 |
| BB vs BTN | 6.5 / 37.4 / 43.9 | 13.4 / 44.6 / 58.1 |
| BB vs SB | 8.4 / 43.6 / 52.0 | 16.3 / 49.6 / 65.9 |
| BB vs BTN, heads-up | 8.4 / 57.8 / 66.2 | 16.9 / 55.7 / 72.5 |

- **Acceptance:** BB vs a CO open, the advice 3-bets 11.5% (was 4.5%; band 8-15%), 72o folds, AA 3-bets, every 3-bet to 133 or 160 (at least twice the open). BB vs a BTN 2.5 BB open defends 58.1% (13.4% 3-bets to 160, 44.6% calls; was 43.9% with 6.5%), and against a 2 BB open the same 58.1% (3-bets to 130). BTN vs CO 3-bets 9.5%, SB vs BTN 14.2%.
- **Sanity, 100 BB first in:** the open ranges are unchanged (UTG 15.8%, HJ 20.4%, CO 27.9%, BTN 46.0%, SB 42.7%, every class per `advisor_chart`), but the sizes moved, because the blinds now defend wider: UTG 2 / 2.25 / 2.9 / 3.5 BB 8.4 / 3.2 / 0.5 / 3.8% (was 8.0 / 4.8 / 0.5 / 2.6), HJ 9.4 / 4.5 / 0.8 / 5.7, CO 11.5 / 5.4 / 2.1 / 8.9, BTN 11.5 / 17.8 / 5.0 / 11.8 (was 2.4 / 30.5 / 5.0 / 8.1), SB 2 / 2.5 / 3 BB 0.6 / 15.1 / 27.0. The 3.5 BB size tell with strong hands (batch C) is a little larger. 12 BB first in is still exactly the push/fold tables.
- **Found, not fixed: min-3-bets.** The preflop ladder's EVs are flat within about 0.5-4 chips from 1.7x to 3.6x for medium hands, and the smallest rung (83 over an open of 50) wins some: HJ vs UTG with AQs, KQs, AQo, A5s and A4s (AQs: 53.3 at 83, 53.2 at 115, 49.4 at 148), and, before they were moved to the flats, AJs and AJo from the BTN against the CO and AJs from the CO against the HJ (AJs: 58.1 at 83 against 57.6 at 115). Below the standard size the opener's calls grow by ratio^-1, so a cheap 3-bet gets called more by hands it beats; position and postflop play are not counted. Every other spot measured 3-bets at least twice the open.

**2. A deep jam into a short Hero** (`advisor_charts.{h,cpp}`)
- Facing a lone all-in, push/fold now applies only when the jam is short: its depth, read as the range model reads it, is at most 15 BB. Jam depth = min(the jammer's street total, the deepest starting stack among the other players dealt in who had not folded when it was made, Hero included). So a SB jam of 100 BB into a 10 BB BB after everyone folded is a 10 BB jam (table), but a BTN jam of 100 BB with a 100 BB SB still behind is a 100 BB jam.
- A deeper jam with Hero's effective stack at most 15 BB gets no chart (`AdvisorChart::neutral`, no prior): the range EV against the jammer's commit range (JJ+, AK at 100 BB) decides. Hero deeper than 15 BB keeps the old path (`vs_four_bet_response` when the call commits).
- Why no prior rather than the commit chart (JJ+, AK as a call range): Hero's price depends on Hero's stack, and the fixed chart is wrong at one end. The reviewer's spot (BB 10 BB, SB 100 BB, BTN jams 100 BB), model call EVs in chips: 22 -50.0, 55 -43.9, 99 -43.8, TT -42.0, JJ -30.0, QQ +14.1, KK +76.7, AA +162.1, AKo -16.8, AKs -4.6, AQs -48.7 (the call needs 180 / 410 = 43.9%; AKo has 39.8%). The commit chart would add +65 to JJ and AK and call them at -30 and -17. With a 3 BB Hero the price is 40 / 130 = 30.8%, and 22-TT, AQs and AK are all +1 to +16: the commit chart would fold 22-TT and AQs at -30. Before, 22 and 55 called (prior +65 on a model -50 and -43.9).
- Acceptance: in the reviewer's spot 22 and 55 fold and QQ, KK, AA call (`DeepJamIntoAShortStackHasNoChartPrior`, which also checks that the call EV is exactly the range EV and that with the SB at 10 BB the table applies again).

**3. Folders hold their fold parts** (`training_advisor.cpp`, `call_value` and `RangeEquities::with_ranges`)
- Each scenario of the players still to act deals its own hands: the certain players from their ranges, the callers from their call ranges and the folders from their fold parts (range x P(fold), so they hold cards but do not contest). A set of one opponent stays the exact per-combo average, which leaves the folders' cards out. Each scenario with a multiway set gets its own Monte Carlo (seed from the line's and the scenario), P(S) over the sum of P over those scenarios of the line's rollouts (capped at 30,000 x the scenario's 512-deal acceptance), at least 100.
- Hand checks, call EV without the chart prior (the reviewer's two spots; "folders out" deals only the scenario's players, as the reviewer did):

| Spot | Before (Stage 6) | Now, seed of the check | Now, 20 seeds | Folders from call ranges, same ranges, 20 seeds | Reference: folders out | Folders from fold parts | As the model deals |
|---|---|---|---|---|---|---|---|
| CO AcQd vs a 10 BB UTG jam | +18.1 | +21.2 (48.7%) | +21.8 ± 0.9 | +19.1 ± 1.1 | +22.9 | +23.8 | +21.8 (in-test) |
| UTG AcQc on 2c7dJh, BB bets 100 into 210, CO and BTN behind | -21.7 | -11.9 (20.8%) | -12.5 ± 2.2 | -11.8 ± 2.9 | -12.1 | -14.7 | about -13.4 |

  - The AQo ranges are push/fold ranges, unchanged by fix 1. The model is 1.7 chips under the reviewer's +22.9, but within 0.6 of a reference that scores the nobody-calls scenario as the model does: about 0.6-1.4 chips of the gap is the class matrix there (AQo against the UTG range 53.80% against 53.97% exact, 54.22% with the folders' cards out), and the rest Monte Carlo noise (sd 0.9).
  - The flop spot's ranges changed with fix 1 (the CO and BTN flat a UTG open from the advisor's tables and the BB defends 32%), so the reviewer's -17.7 (on the old ranges) no longer applies; the same reference on the new ranges is -12.1, and -14.7 with every folder dealt. The model's -12.5 ± 2.2 sits between them; leaving the folders out of the heads-up scenario is worth about +1.3 here (the folders' fold parts hold AQ's outs: AQ's heads-up equity against the BB is 31.9% exact and 30.1% with them dealt). In this spot the old treatment differed little on the new ranges (-11.8).
- Latency: below.

**4. Inferred calls respond to raise size** (`range_model.{h,cpp}`)
- The walk records each preflop raise's size_ratio, as the advisor does (`standard_preflop_raise_to`, now public in `range_model.h`, shared by both). A call that is not all in and does not commit uses `call_size_scale(size_ratio)` as the prediction does: Cc resized by min(1, f), and below the standard size `add_calls_for_size` on the range so far. A limper's call scales its call width the same way. Measured: the BB's flat of a BTN open of 2 / 2.5 / 3 BB holds 59.1 / 47.3 / 43.4% of combos, slowplayed 3-bets included (before: 47.3% at every size), each exactly the predicted call range (`InferredFlatsRespondToRaiseSize`).
- **The standard 3-bet out of position is 3.5x** (a 3-bettor who acts first after the flop, as the blinds do), 3x in position: the sizes the positional vs-3-bet tables assume ("3x in position, 3.5-4x from the blinds"). With the bots' 3x everywhere, the bench's SB 3-bet to 180 (3.6x) read as 1.2x standard, narrowed the BTN's flat, and the bench went to 0.743. Bench by the out-of-position multiple: 3.0 0.743, 3.5 0.694, 3.6 0.691 (the bench's own size, ratio 1), 3.75 0.756. 3.5 was taken as the low end of the tables' stated size, not fitted; the bench is this sensitive because a few SB-vs-BTN 3-bet spots move 1-2 BB on small range changes (dry-xx 2.95 at 3.5 and 3.6, 3.90 at 3.75).
- The advisor uses the same standard for Hero's raises, so its 3-bets from the blinds are bigger now (BB vs BTN and BB vs CO mostly 160, the pot rung).

**5. Known limitation, push/fold rounding (no code change).** The multiway push/fold sets are rounded from fictitious play's mixed averages (each class whose average is at least 0.5), so the pure sets are slightly off a best response to each other: in the review's check (UTG-BTN at 4, 8, 10, 12 and 15 BB), 0-9 jam classes and 1-15 call classes per game sit on the wrong side of indifference, worth up to 0.40 BB per hand of that class (CO and BTN folding KQs to a 12 BB HJ jam; the BB folding A9s to a 10 BB UTG jam, 0.25), about 1 mBB per hand dealt. Heads-up and SB-vs-BB sets are within 0.32 mBB.

**River bench** (`advisor_bench`, defaults): overall **0.694 BB, 57% agree** (was 0.708, 56%). BTN vs BB 0.288 → 0.320, CO vs BTN 0.435 → 0.421, SB vs BTN 3-bet 1.402 → 1.342; dry 0.695, wet 0.342, paired 0.675, monotone 1.065. Fix 1 alone gave 0.690; the bench's ranges are fixed, but the advisor reads the opponents' preflop ranges through the walk.

**Hint latency** (`Advisor.DISABLED_LatencyByStreet`, Release, default options, ms, p50 / p95 / max, run alone)

| Spot | Lines off, auto threads | Lines on, auto threads | Lines off, 1 thread | Lines on, 1 thread |
|---|---|---|---|---|
| Preflop 3+ way (2010) | 3.4 / 7.6 / 17 | 6.5 / 10.1 / 19 | 4.5 / 7.4 / 17 | 14.9 / 26.2 / 59 |
| Flop 3+ way (1774) | 6.6 / 11.4 / 19 | 8.5 / 12.7 / 24 | 9.7 / 20.9 / 30 | 21.8 / 34.9 / 51 |
| Flop heads-up (300) | 4.1 / 4.7 / 5.5 | 5.0 / 6.0 / 8.1 | 11.7 / 12.4 / 19 | 13.8 / 14.6 / 18 |
| Turn 3+ way (1588) | 4.3 / 8.2 / 19 | 5.8 / 10.5 / 33 | 4.9 / 8.8 / 42 | 11.3 / 23.9 / 60 |
| Turn heads-up (300) | 1.5 / 1.7 / 2.6 | 2.1 / 2.8 / 4.5 | 1.5 / 1.6 / 2.3 | 2.5 / 2.7 / 3.5 |
| River 3+ way (1434) | 4.1 / 8.4 / 114 | 5.3 / 11.1 / 110 | 4.4 / 8.8 / 260 | 10.6 / 23.8 / 284 |
| River heads-up (300) | 1.3 / 1.5 / 2.3 | 1.8 / 2.2 / 2.6 | 1.3 / 1.4 / 2.7 | 2.8 / 3.0 / 4.1 |

- With automatic threads every 3+ way p95 is at most 12.7 ms (target 15): preflop 10.1 (was 8.2), flop 12.7 (11.3), turn 10.5 (9.0), river 11.1 (9.7). The preflop rise is the per-scenario Monte Carlo runs (most below the threading threshold, so inline) and the wider blind ranges (more scenarios with a caller). One thread with the lines on is slower than before (preflop p95 26 against 17.9), as each scenario now has its own run.

**Tests:** 453 pass in Release (7.8 s) and Debug (30.3 s), 447 + 6 new; GCC Release/Debug and clang build with zero warnings; `strict.sh` is clean on `preflop_charts.cpp`, `advisor_charts.cpp`, `training_advisor.cpp` and `range_model.cpp`. Each new test takes at most 0.14 s in Release (0.35 s in Debug).

## Unverified

- MSVC `/W4` builds and the Windows executable.
- Windows memory detection for solver tiers (`GlobalMemoryStatusEx`).
- Real GLFW mouse and keyboard input, which the spec's existing known limit already covers.
- The combo tables (`combos.cpp`) are built at run time behind a function-local static rather than with `constexpr`, to stay clear of MSVC's constexpr step limit. Whether MSVC would actually hit that limit wasn't tested.
- The multiway redraw cap (16,384) and the flop threading threshold (64 needed combos) were chosen, not tuned.
- Timings come only from this container.
- Solver solves are single-threaded so far. The `threads` limit is validated but not yet used, so thread-count determinism has nothing to test until Stage 8.
- Flop trees are built before the memory check runs. Stage 8 has to count nodes before building a flop tree.

- Stage 5: the preflop all-in rule's 15 BB line was chosen to match the range model's short-stack threshold and Stage 6's push/fold range; it was not measured.
- Stage 5: the 3+ way hint latencies come from random play, whose lines are not representative of real games.
- Stage 5 batch B: the archetype lines are not checked against a solver or against the bots; the river bench scores Baseline only. The exploit directions in the examples are judgment.
- Stage 5 batch B: the line Monte Carlo budget (a third of the rollouts, 150,000 deals, 512 trial deals) was chosen for latency; its effect on line advice was not measured beyond the 1-2 pp noise estimate. Spots where no trial deal fits use the independence product, whose error there is unmeasured.
- Stage 5 batch B: Hero's Baseline-read range as the bettor range for every line was not compared with a per-type reading.
- Stage 5 batch B: the threads used for the lines were run under GCC's ThreadSanitizer only for the advisor, range-equity and controller tests (Stage 5 review); clang's TSan and MSVC were not available.
- Stage 5 batch D: the tuned Baseline parameters were fitted to the river bench's hand-written ranges. The winner sits at a corner of the grid, the grid was not extended, and the turn holdout (Stage 8) does not exist yet. Their effect on flop and turn advice was checked only through the Stage 4 plausibility tests and the unchanged test suite. (The three parameters are postflop only, so preflop advice does not change.)
- Stage 5 batch D: the diagnosis of the 3-bet-pot spots compares the model's inferred lead range with the bench's assumed range. Neither range is solved, so which story is closer to real play is unverified.

- Stage 6 batch A: the positional 3-bet and 4-bet tables are hand-authored to the stated bands, in the style of published 100 BB 6-max charts. They were not solved or compared with a specific solver output.
- Stage 6 batch B: the 6-max push/fold tables rest on the one-caller approximation, and they were not compared with a full multiway equilibrium (such as HRC). Only the heads-up game is checked against published numbers, and only at 10 BB.

- Stage 6 batch C: the players-still-to-act model treats each player's continue as independent and lets a folded player hold cards from its call range in the Monte Carlo (heads-up scenarios use the exact per-combo vector, so only the multiway ones carry this; in the AQo spot it moves AQo's heads-up equity from 53.8% to 51.0% when the call ranges of two folded players are dealt, so the multiway scenarios are biased by a similar order); an overcall of a short jam uses the lone-call push/fold range, and the original bettor never re-raises after a squeeze. None of these was measured against a multiway solve.
- Stage 6 batch C: the continuing Monte Carlo's 30,000-deal budget was chosen for latency. Spots where it bites (acceptance below 10%) get fewer rollouts, down to 100; their error was not measured.
- Stage 6 batch C: the 15-25 BB blend (linear, into the commit range for jams and the chart answer for predicted calls) and the size exponents (ratio^-1 below standard, ratio^-0.5 above) are judgment, not fitted. The BB's defend shares against 2 and 2.5 BB opens (55% and 45% with 3-bets) look a little tight next to published solver outputs; not compared with any.
- Stage 6 batch C: `implied` 0 preflop was checked through the 169-class spots and the test suite only; its effect on preflop mistakes in real play was not measured. The AQo call EV was checked approximately by hand (+24 against the model's +18.1), not exactly.

- Stage 6 follow-ups: the advisor's vs-open tables are hand-authored to the stated bands, in the style of published 100 BB 6-max outputs; they were not solved or compared with a specific solver. The cold callers' flats and the SB's pure 3-bet-or-fold are simplifications.
- Stage 6 follow-ups: the 3.5x out-of-position standard 3-bet was chosen from the tables' stated size; the bench was checked at 3.0, 3.5, 3.6 and 3.75 only, and its sensitivity there (0.69-0.76) is larger than any principled reason to prefer one of them.
- Stage 6 follow-ups: the per-scenario rollout split (by chance, at least 100) was chosen for latency; the hand checks' seed spread (sd 0.9 and 2.2 chips) was measured only in the two review spots.
- Stage 6 follow-ups: the multiway push/fold sets are rounded from mixed strategies (fix 5), up to 0.40 BB per hand for the worst classes, about 1 mBB per hand dealt; not corrected.

## Test changes

(Each entry gives the test name, the old assertion, the new assertion and the reason.)

- **Stage 1:** no existing test changed; all new tests are in `tests/range_test.cpp`.
- **Tolerance note:** `PreflopMatrix.SpotChecksAgainstMonteCarlo` uses max(3 combined standard errors, 0.6 pp). Three standard errors is about 0.43 pp there, so the 0.6 pp floor is a little looser than the plan's "within 3 standard errors". It was kept so the test can't be flaky.
- **Stage 4 review fixes** (`tests/range_model_test.cpp`):
  - `RangeModel.BluffShareTracksSize`:
    - **Old:** the BTN's river bet after checks equals prior × `bet_breakdown(prior, ...)` (own-range rule), and the measured air share (top_share > 0.6 in the prior) is within 0.05 of s/(1+2s).
    - **New:** the BB's river lead after the flop and turn are checked through equals prior × `bet_breakdown(prior, ..., &btn_range)`. It is viewed from a folded player without card removal, so the BTN range given is the walk's own. `bet_likelihood` agrees, the bluff weight stays below the 0.9 cap, and air is measured against the BTN's range (u > 0.6). Same tolerances.
    - **Reason:** inference now judges value against the opponent. The BTN's own checked range holds too little air to calibrate, because as the flop aggressor it bluffed most of it.
  - `RangeModel.ThreeBetAndFourBetUseTheCharts`: a comment fix only (the viewer is the BTN, not the BB).
  - **New tests:** `CheckRaiseRangesAreStrong`, `CbetFrequenciesArePlausible`, `PredictedRaisesAreJudgedAgainstTheBettor`, `LimpCallKeepsASensibleRange`, `FallbackNeverRevivesPreflopFolds`, `WidthsOrderByTypeHeadsUpAndByPosition`, `ShortAllInIsNotANewRaiseLevel`. Each takes ≤ 0.015 s in Release.

- **Stage 5 batch A** (`tests/training_advisor_test.cpp`):
  - `Advisor.EvFormulasMatchTheDocumentedModelForCheckAndCall`:
    - **Old:** check = `equity_pct` × 200; call = (`equity_pct` − 0.075) × 500 − 150.
    - **New:** check = `range_equity_pct` × 200; call = `range_equity_pct` × 500 − 150.
    - **Reason:** the EVs use range equity, and the advisor no longer applies `bet_pressure`.
  - `Advisor.RaiseEvsMatchTheDocumentedModel`:
    - **Old:** each rung and the all-in equal the MDF and floor formula with exponent fudges on `equity_pct`.
    - **New:** each equals F·200 + P_call·(e_c·(200 + 2a) − a + implied) + P_raise·max(−a, e_r·(200 + 2·R2) − R2), with R2 = min(200 + 3a, 1900), rebuilt from `outcomes` (within 1e-9).
    - **Also new:** some of the range folds to each size; the set has more than 50% against the calls; and the fold share grows from 25% to pot to all-in.
    - **Reason:** the new model.
  - `Advisor.RiverNutsRaiseEvsAreExact`:
    - **Old:** all-in = 0.9 × 200 + 0.1 × 2100 (the 10% call floor); 75% = (1 − 1/1.75) × 200 + (1/1.75) × 350.
    - **New:** `range_equity_pct` and both outcome equities are 100. All-in = F × 200 + (1 − F) × 2100. 75% = F × 200 + P_call × 350 + P_raise × 850 (a pot-sized re-raise to 650 called), with F and the shares from `outcomes`.
    - **Reason:** fold and raise shares now come from the range model.
  - `Advisor.CallEvCountsEachPotAgainstItsOwnOpponents`:
    - **Old:** the BTN limps, the SB completes, the BB shoves 40, both call, the river bet is 1000. Expected = ((120·e₂ + 2000·e₁)/2120 − 0.15) × 2120 − 1000 with random-hand equities, and the advice is Fold.
    - **New:** the BTN opens to 60, the SB calls, the BB calls all in for 40, the river bet is 1000. Expected = Σ layer amount × equity_vs_ranges(`infer_ranges`, that layer's villains, 200,000 rollouts) − 1000, within 5 chips. The test also checks the layers (120 chips 3-way, 2040 heads-up), that the heads-up layer has the higher equity (2.4% against 0.3%), and that the advice is still Fold.
    - **Reason:** in the old line the Baseline SB's completing range is a 2% limp window, narrowed to about 2 combos by the river bet, all beating Q5o. Both layers were 0, so the test no longer checked the layering. The pressure term is gone.
  - `Advisor.DoesNotShoveAirOrTopPairDeep`:
    - **Old:** 54o checked to on Kd8s3c checks.
    - **New:** it never shoves, and any bet is at most 50 (25% pot).
    - **Reason:** against ranges, a 25% stab wins 43.5 against 36.9 for checking, from 24% folds (the Baseline continues 0.95 × MDF). That is the known fold-equity limit in CLAUDE.md section 12, not a shove. The other two assertions are unchanged and pass.
  - `Advisor.FieldsInRangeAndBestActionLegalAcrossRandomStates`:
    - **Old:** `TrainingAdvisor(12, 100)`.
    - **New:** fast options {100, 500, 1, 1, false}, plus the outcome invariants (`expect_consistent_outcomes`): outcomes are listed for exactly the listed raise rows; fold + raise ≤ 100; equities are in [0, 100]; `range_equity_pct` is in [0, 100]; `versus` is empty.
    - **Reason:** speed (an exact flop per state). No bound changed.
  - `AdvisorSizing.EverySizeIsUsedAndAdviceStaysSane`:
    - **Old:** `TrainingAdvisor(43, 300)`.
    - **New:** {300, 500, 1, 1, false} plus the same outcome invariants. Every bound is unchanged (each rung used, none above 70%). The distribution is under Measured results.
    - **Reason:** speed.
  - **Unchanged and still green:** `DifferentDecisionsGetDifferentRollouts`, `AdvisorSizing.EachSizeIsRecommendedInItsSpot` (no spot needed replacing), the 169-class chart tests, `Mistakes.RealAdvisorFlagsFoldingAcesPreflop`, `Controller.PlayingEveryHintLogsNoMistakes` and `LoggerFiles.FullSessionProducesConsistentLogs`.
  - **New tests:**
    - `RangeEquityBelowRandomFacingRiverPotBet`: K4o check-calls the flop and turn and faces a pot-sized river bet. Random-hand equity is above 70%, range equity is at least 20 pp lower, and the rationale names both.
    - `OutcomesAreConsistent`: checked to, facing a bet, first in preflop, and facing a covering all-in (no outcomes). Fold shares grow and equity when called shrinks with size.
    - `OptionsAreValidated`: rejected options and the delegating constructor's values.
    - `ThreadCountDoesNotChangeTheHint`: 1 and 3 threads on a multiway flop give bit-identical hints.
    - `DISABLED_LatencyByStreet`: a report.
    - Each new test takes ≤ 0.03 s in Release.

- **Stage 5 batch B** (`tests/training_advisor_test.cpp`):
  - `expect_consistent_outcomes` (used by `OutcomesAreConsistent`, `FieldsInRangeAndBestActionLegalAcrossRandomStates` and `EverySizeIsUsedAndAdviceStaysSane`):
    - **Old:** `versus` is empty.
    - **New:** `expect_consistent_lines`: either no lines or all three in order (Nit, Station, Maniac); each line's action legal and its listed best; bets on the hint's ladder; EV rows and outcomes present exactly where Baseline's are; shares and equities in range; `why` ≤ 140 characters and starting "Same as Baseline: " or "vs <Type>: ".
    - **Reason:** default options now fill the lines (`OutcomesAreConsistent` uses them); the fast-options loops still have lines off.
  - `Advisor.ThreadCountDoesNotChangeTheHint`: also compares every line's action, range equity, two EV rows and `why` between 1 thread (serial lines) and 3 (parallel). Nothing relaxed.
  - `Advisor.DISABLED_LatencyByStreet`: now loops lines off and on, each with automatic threads and one thread.
  - **New tests:** `ArchetypeLinesAreLegalAndDeterministic`, `BaselineHintUnchangedByArchetypeLines` (four views, every Baseline field compared with `==`), `VsStationBluffsLessAndValueBetsThinner` (5c4d flop: Baseline bets, Station checks with ≥ 10 pp fewer folds; As3d river: Station bets, Nit checks; KsQh flop: Station 150, Nit 50, Baseline 100), `VsNitFoldsMoreToBigBets` (As3d river: Nit folds more to every size; facing a pot bet Baseline calls, Nit folds below pot odds), and the report `DISABLED_PrintArchetypeLines`.

- **Stage 5 batch D:** no test assertion changed. With the new Baseline defaults all 428 tests pass in Release and Debug, including the archetype and range-model tests. The Stage 4 plausibility numbers moved within their bounds (see "Stage 5 tuning"); the closest is `CheckRaiseRangesAreStrong`'s flop check-raise, 47.7% against a lower bound of 45%.

- **Stage 5 review:** no assertion changed. New test `Advisor.RaiseEvsCountWhatShortStacksCanMatch`: rebuilds the bet/raise EV per villain from the range model's public functions, at 1e-6, for a raise and an all-in beside a short all-in player and for an all-in and a pot bet against a villain too short to cover it. It fails on `0e388be` and takes about 0.02 s in Release.

- **Stage 6 batches A and B:** no existing assertion changed. New tests:
  - In `tests/preflop_charts_test.cpp`: `PositionalVsThreeBet.NestsSensibly` (continue-share bands per table, continue ⊆ RFI for every opener/3-bettor pair, raise and call disjoint, in-position call and continue ⊇ out-of-position for UTG/HJ/CO), `PositionalVsThreeBet.EarlierOpenersKeepMoreOfTheirOpens`, `PositionalVsThreeBet.KnownDecisions`, `PositionalVsFourBet.VsFourBetRangesAreNarrow`, and `VsReraises.BotsChartsUnchanged` (a 169-class snapshot of the bots' two charts, taken before the change).
  - In the new `tests/push_fold_test.cpp`: `HeadsUpTenBbMatchesKnownNash`, `JamShareFallsAsStacksDeepen`, `EarlierPositionsJamTighter`, `CallRangesAreSubsetsOfStrongHands` (premiums called, 72o/32o never at 6 BB or more, each call set a threshold on equity against the jam within 0.02), `GeneratedTableMatchesARecomputation` and `ResponsesAndNulloptCases`.
  - `tests/player_boundary_test.cpp` also includes `strategy/push_fold.h`.

- **Stage 6 batch C:**
  - `RangeModel.ThreeBetAndFourBetUseTheCharts`:
    - **Old:** the CO's 4-bet over the BTN's 3-bet is `vs_three_bet_response` Raise ∩ the CO's RFI at 1 - slowplay; after the BTN calls the 4-bet, QQ 0.8, AA 0.16 and TT 0.
    - **New:** the CO's 4-bet is `vs_three_bet_ranges(CO, BTN).raise` ∩ RFI at 1 - slowplay; the BTN's flat is checked combo by combo (board removed) as (1 - slowplay on its 3-bet range) × (slowplay on the `vs_four_bet_ranges(BTN, CO)` jam, 1 on its call, else 0). QQ 0.8, AA 0.16 and TT 0 still hold.
    - **Reason:** the opener and the 3-bettor now answer from the positional tables.
  - `RangeModel.ShortAllInWidensAsStacksShrink`:
    - **Old:** a 10 BB BTN shove is 40% ± 2 of combos (the temporary 400/BB formula); the BB's call of it is exactly the top 25%.
    - **New:** the 10 BB shove's mass equals the BTN's 10 BB jam table (32.1%); the BB's call equals its push/fold call range against the BTN at 10 BB. The ordering and deep-shove assertions are unchanged.
    - **Reason:** the push/fold tables replace the formulas.
  - `RangeModel.CbetFrequenciesArePlausible`:
    - **Old:** the BB's 3-bet range c-bets a third of the pot at least 85% of the time on K83r against the UTG's flat.
    - **New:** at least 70% (measured 77.7%, was 89%). The single-raised-pot bound (60-80%) is unchanged.
    - **Reason:** the UTG's positional flat of a blind 3-bet holds QQ-88, AKo, AQs-ATs, KQs, KJs, KTs and suited connectors, so the 3-bettor no longer beats almost all of it; the comment was rewritten to match.
  - Oracle switch: `Advisor.FacingAShoveAgreesWithTheChartEverywhere`, `Advisor.FacingANormalOpenFollowsTheChartOnWhetherToPlay` and the `chart_response_for_shove` helper use `advisor_chart_response` instead of `chart_response`. No assertion changed; the new `AdvisorChartMatchesTheBotsChartOutsideItsOwnTables` shows the two agree on all 169 classes in a 100 BB single-raise spot.
  - The `six_max` helper takes an optional stack (default 2000); no assertion changed.
  - **New tests:** `RangeModel.ShortAllInUsesPushFoldTables` (the jam table at 3, 5, 10 and 15 BB, the 16-25 BB blend class by class, falling mass, 16 BB above 85% of 15 BB, the commit range at 25 and 100 BB, a re-jam over the CO, the SB's call and the BB's overcall), `RangeModel.SmallRaisesGetCalledMore`, `Advisor.AdvisorChartMatchesTheBotsChartOutsideItsOwnTables`, `Advisor.ShortStackFirstInIsPushOrFold` (four positions at 10 BB, and no push/fold at 16 BB), `Advisor.ShortStackCallsAJamPerTable`, `Advisor.BbVsCoOpenThreeBetShareIsPlausible` (BB vs CO between the chart's 4.5% and 15%, 72o folds and AA 3-bets, no 3-bet below twice the open; BTN vs CO 4-15%, SB vs BTN 8-20%), and `Advisor.PlayersBehindAreNotCertainCallers` (the AQo jam spot calls, within 7 pp of heads-up and more than 10 above four-way; the AJs spot plays, within 7 pp below heads-up, every line more than 5 pp above its three-way equity; a multiway flop call above the equity against all three).
  - `tests/player_boundary_test.cpp` also includes `advisor/advisor_charts.h`.

- **Stage 6 follow-ups:**
  - Oracle switch to `advisor_vs_open_ranges` (the range model now reads one raise from it): `RangeModel.BbDefendVsBtnIsChartCallPlusTraps`, `RangeModel.PredictPreflopResponseShrinksWithSize`, `RangeModel.SmallRaisesGetCalledMore` (also a comment: 0.53 is of a 3x 3-bet) and the BTN's 3-bet in `RangeModel.ThreeBetAndFourBetUseTheCharts`. No bound changed in them.
  - `RangeModel.ThreeBetAndFourBetUseTheCharts`:
    - **Old:** after the BTN calls the CO's 4-bet, TT has weight 0.
    - **New:** 99 has weight 0.
    - **Reason:** TT is now in the BTN's 3-bet range against the CO and in the in-position call range against a 4-bet, so it flats at 0.8; 99 is outside the 3-bet range. The per-combo comparison is unchanged.
  - `RangeModel.UsersSpotIsPlausible`:
    - **Old:** 88 on Ah9s4c has 40-55% against the BB's defend range.
    - **New:** 40-62% (measured 59.4%).
    - **Reason:** the BB defends 58% of hands against the BTN, many of them weak offsuit, so 88 is a favourite. The check-call bound is unchanged (46.8%).
  - `RangeModel.CbetFrequenciesArePlausible`:
    - **Old:** the BB's 3-bet range c-bets a third of the pot on K83r at least 70% of the time.
    - **New:** at least 50% (measured 56%, was 77.7%); comment rewritten.
    - **Reason:** the BB's 3-bet against UTG is polarized now (suited wheel aces and 54s-T9s), and the connectors that miss K83 check some of the time. The single-raised-pot bounds are unchanged (62%).
  - `Advisor.AdvisorChartMatchesTheBotsChartOutsideItsOwnTables` is replaced by `Advisor.AdvisorChartFacingAnOpenUsesItsOwnTable`:
    - **Old:** in a 100 BB single-raise spot (BB vs CO) `advisor_chart_response` equals the bots' `chart_response` for all 169 classes.
    - **New:** it equals `advisor_vs_open_ranges(BB, CO)`'s answer for all 169 classes, is never push/fold, and differs from `chart_response` in more than 20 classes.
    - **Reason:** the advisor no longer uses the bots' vs-open chart, by design.
  - `Advisor.BbVsCoOpenThreeBetShareIsPlausible`:
    - **Old:** the BB's advised 3-bet share is at least the bots' chart (4.5%) and at most 15%.
    - **New:** 8-15% (measured 11.5%). The BTN (4-15%, measured 9.5%) and SB (8-20%, 14.2%) bounds, 72o folding and the two-times-the-open size floor are unchanged.
    - **Reason:** the plan's band, now that the advisor has its own BB-vs-CO chart.
  - `Advisor.PlayersBehindAreNotCertainCallers`, BTN AJs facing a CO open:
    - **Old:** range equity within 7 pp below heads-up.
    - **New:** within 9 pp (measured 50.7% against 58.3% heads-up; was 52.4%). The "more than 10 pp above three-way" and archetype-line bounds are unchanged.
    - **Reason:** the blinds defend from the advisor's tables (the BB 49% against a CO open, the SB 3-bets 10.7% and flats like the BTN), so the field is three-way about half the time.
  - `VsReraises.BotsChartsUnchanged`: extended with a 169-class snapshot of the bots' ten vs-open tables and a check of which table each hero, opener and table size (6 and 2 players) reads.
  - **New tests:** `AdvisorVsOpen.SharesAreInTheirBands` (3-bet and defend shares per table in the bands above, raise and call disjoint, the SB's call empty), `AdvisorVsOpen.NestsAndIsWiderThanTheBots`, `AdvisorVsOpen.ThreeBetsArePolarizedAndPremiumsAlwaysRaise` (plus known decisions), `RangeModel.InferredFlatsRespondToRaiseSize` (flats of 2, 2.5 and 3 BB opens equal the predicted call ranges, the 2 BB flat wider and containing the standard one; the BTN's flat of a 3.5x SB 3-bet is the ratio-1 answer; `standard_preflop_raise_to` values), `Advisor.PlayersBehindWhoFoldHoldTheirFoldParts` (the AQo call within 2 chips of a reference rebuilt from public functions with 60,000 rollouts per scenario, +21.2 against +21.8, and that reference more than 2 chips above the same with folders dealt from their call ranges, +18.8), and `Advisor.DeepJamIntoAShortStackHasNoChartPrior`.
