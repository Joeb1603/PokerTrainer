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

## Deviations from the spec or earlier statements

- **What I said earlier:** in the conversation that led to the plan, I said "the GTO bot gets [range equity, exact equity, range-based fold equity] automatically".
- **What changed:** that was reversed (decision 1). Section 4 of the spec fixes the GTO bot's method, and the bots also serve as fixed opponents for measuring the exploit advice.
- **A correction to numbers given earlier:** during planning I quoted 8c8d on Ah9s4c as 65.6% against a random hand, and QhJh on Qd9s8h as 82.3%. Those came from only 4,000 Monte Carlo rollouts, about ±0.75 pp. Exact enumeration gives 63.95% and 82.80%, and 2 million rollouts agree. The comparison still holds: the underpair is about 47% against the BB's defend range and about 23% against the part of it that continues.

- **Stage 5, `BetOutcome`:** its fields are percentages (0-100), like the other `_pct` fields of `HintRecommendation`, not the 0-1 shares the batch instructions described. It also has a fourth field, `equity_raised_pct`, because the raise branch of the EV can't be rebuilt or shown without it.
- **Stage 5, preflop all-in rule:** decision 16 is new, a fix to a clear model bug rather than tuning.

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
- Stage 5 batch B: the threads used for the lines were not run under ThreadSanitizer.
- Stage 5 batch D: the tuned Baseline parameters were fitted to the river bench's hand-written ranges. The winner sits at a corner of the grid, the grid was not extended, and the turn holdout (Stage 8) does not exist yet. Their effect on flop and turn advice was checked only through the Stage 4 plausibility tests and the unchanged test suite. (The three parameters are postflop only, so preflop advice does not change.)
- Stage 5 batch D: the diagnosis of the 3-bet-pot spots compares the model's inferred lead range with the bench's assumed range. Neither range is solved, so which story is closer to real play is unverified.

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
