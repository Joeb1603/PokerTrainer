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

## Deviations from the spec or earlier statements

- **What I said earlier:** in the conversation that led to the plan, I said "the GTO bot gets [range equity, exact equity, range-based fold equity] automatically".
- **What changed:** that was reversed (decision 1). Section 4 of the spec fixes the GTO bot's method, and the bots also serve as fixed opponents for measuring the exploit advice.

## Measured results

(Filled in per stage, each with its commit hash.)

## Unverified

- MSVC `/W4` builds and the Windows executable.
- Windows memory detection for solver tiers (`GlobalMemoryStatusEx`).
- Real GLFW mouse and keyboard input, which the spec's existing known limit already covers.

## Test changes

(Each entry gives the test name, the old assertion, the new assertion and the reason.)
