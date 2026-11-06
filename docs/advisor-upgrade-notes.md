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

## Deviations from the spec or earlier statements

- **What I said earlier:** in the conversation that led to the plan, I said "the GTO bot gets [range equity, exact equity, range-based fold equity] automatically".
- **What changed:** that was reversed (decision 1). Section 4 of the spec fixes the GTO bot's method, and the bots also serve as fixed opponents for measuring the exploit advice.
- **A correction to numbers given earlier:** during planning I quoted 8c8d on Ah9s4c as 65.6% against a random hand, and QhJh on Qd9s8h as 82.3%. Those came from only 4,000 Monte Carlo rollouts, about ±0.75 pp. Exact enumeration gives 63.95% and 82.80%, and 2 million rollouts agree. The comparison still holds: the underpair is about 47% against the BB's defend range and about 23% against the part of it that continues.

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
- **Fallback:** a range falls back to the last action's likelihood only when it drops below 1% of combos *and* below 5% of its prior mass. This keeps small but legitimate ranges, such as a Nit's 4-bet range.

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

## Unverified

- MSVC `/W4` builds and the Windows executable.
- Windows memory detection for solver tiers (`GlobalMemoryStatusEx`).
- Real GLFW mouse and keyboard input, which the spec's existing known limit already covers.
- The combo tables (`combos.cpp`) are built at run time behind a function-local static rather than with `constexpr`, to stay clear of MSVC's constexpr step limit. Whether MSVC would actually hit that limit wasn't tested.
- The multiway redraw cap (16,384) and the flop threading threshold (64 needed combos) were chosen, not tuned.
- Timings come only from this container.
- Solver solves are single-threaded so far. The `threads` limit is validated but not yet used, so thread-count determinism has nothing to test until Stage 8.
- Flop trees are built before the memory check runs. Stage 8 has to count nodes before building a flop tree.

## Test changes

(Each entry gives the test name, the old assertion, the new assertion and the reason.)

- **Stage 1:** no existing test changed; all new tests are in `tests/range_test.cpp`.
- **Tolerance note:** `PreflopMatrix.SpotChecksAgainstMonteCarlo` uses max(3 combined standard errors, 0.6 pp). Three standard errors is about 0.43 pp there, so the 0.6 pp floor is a little looser than the plan's "within 3 standard errors". It was kept so the test can't be flaky.
