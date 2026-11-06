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
- Every spot is solved to ≤ 0.3% of pot exploitability, with at least 200 iterations.
- **Hero combos:** up to 80 per spot, sampled evenly across river hand strength.
- **Regret:** the best `ev_best` minus the `ev_best` of the advisor's action, in BB. `ev_best` is the best-continuation EV.
- **Size mapping:** the solver tree uses `all_in_merge = 1.0`, so every advisor size maps exactly.
- **Cost:** a full run takes 2.1 s, of which 1.0 s is solving. The advisor takes p50 0.35 ms and p95 0.40 ms per river decision.

**Baseline: the current random-hand advisor, measured at `dd9e9ed` plus the bench**

| Group | Regret (BB) | % of pot | Regret > 0.5 BB | Regret > 1 BB | Agrees with solver's top action | Solver's frequency for advised action | Advised action used < 10% |
|---|---|---|---|---|---|---|---|
| **Overall (36)** | **1.992** | 5.60 | 50% | 40% | 28% | 28% | 66% |
| BTN vs BB | 1.009 | 4.79 | 46% | 32% | 26% | 25% | 67% |
| CO vs BTN | 1.439 | 5.85 | 50% | 40% | 26% | 26% | 68% |
| SB vs BTN 3-bet | 3.527 | 6.16 | 53% | 48% | 33% | 32% | 62% |
| Dry / wet / paired / monotone | 1.76 / 1.78 / 2.28 / 2.15 | | | | 30 / 20 / 32 / 30% | | |
| OOP first to act | 1.046 | 3.97 | 42% | 29% | 23% | 22% | 68% |
| IP facing 33% | 1.905 | 5.68 | 45% | 36% | 32% | 31% | 61% |
| IP facing 75% | 3.024 | 7.16 | 62% | 55% | 30% | 30% | 68% |

By line, regret is 1.41 BB for check-check, 1.33 for bet then check, and 3.24 for bet-bet.

**Patterns, consistent with random-hand equity overrating hands on the river**
- **It bets too often when first to act, and too big.** On one monotone board the solver checks 92% and the advisor bets 84%.
- **It raises with bluff-catchers when facing bets.** On a paired board in the 3-bet pot it raises 67% where the solver raises 2%, and QQ raises for −7.2 BB where calling is worth +8.8 BB. On one wet bet-bet line it shoves every hand where the solver folds 49%.
- **It rarely folds.** One spot has the solver folding 73% and the advisor 21%.
- **Agreeing is nearly free:** when the advisor matches the solver's top action, its mean regret is 0.002 BB. That supports measuring regret on `ev_best`.
- **The plan's acceptance target for Stage 5:** mean regret ≤ 1.49 BB (0.75 × the baseline), and agreement ≥ 38%.

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
