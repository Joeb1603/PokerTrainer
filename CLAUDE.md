\# Project Specification: 6-Max Texas Hold'em Poker Trainer (C++20)



\## 1. Role \& Objective

You are a Lead C++ Systems Architect. Build a modular, cross-platform C++20 6-Max No-Limit Texas Hold'em Poker Training Application that compiles to a standalone Windows `.exe` (using CMake + MSVC). 

Design the architecture with strict separation between the Core Game/Bot Engine and the Presentation Layer so the local GUI can later be replaced by a REST/WebSocket server without modifying engine code.



\## 2. Technical Stack

\- \*\*Language:\*\* C++20 (standard library containers, `<random>` with `std::mt19937\_64`, `std::optional`, `std::variant`).

\- \*\*Build System:\*\* CMake (`CMakeLists.txt` with `FetchContent` for dependencies).

\- \*\*Hand Evaluator:\*\* Integrate `HenryRLee/PokerHandEvaluator` (`phevaluator`) via FetchContent or a fast 7-card lookup evaluator returning integer hand ranks (1 = Royal Flush to 7462 = worst high card).

\- \*\*JSON Serialization:\*\* `nlohmann/json` for state export and compact JSONL hand logging.

\- \*\*GUI (v1):\*\* `Dear ImGui` (with GLFW/OpenGL3 or DirectX11 backend) for a clean, responsive 6-max table layout, action controls, and toggleable GTO Training Hints panel.



\## 3. Core Game Rules \& Freezeout Structure

\- \*\*Format:\*\* 6-Max No-Limit Texas Hold'em (1 Human Player "Hero" + 5 AI Bots).

\- \*\*Setup:\*\* Configurable starting stacks (default: 2000 chips = 100 BB) and fixed blinds (default: 10/20).

\- \*\*Freezeout Policy:\*\* No re-buys or respawns. When a bot reaches 0 chips, its seat is marked `BUSTED` and skipped in dealing/blinds. If Hero reaches 0 chips (or busts all 5 bots), display the session summary and offer a "Start New Game" button that resets seats and stacks.

\- \*\*Full NLHE Rules:\*\* Accurate dealer button rotation, heads-up blind rules when down to 2 players, minimum legal raise sizing, all-in short-stack protection, and multi-way side-pot calculation and distribution.



\## 4. Bot Architecture \& 5 Personalities

Implement an `IBotStrategy` interface driven by a shared `GtoHeuristicEngine` and parameterized `PersonalityProfile`:

1\. \*\*Bot 1 - "Nit" (Overly Cautious):\*\* Plays top 12% preflop hands. Postflop requires `Equity >= PotOdds + 0.15` to call; rarely bluffs (`BluffFreq = 0.03`); folds non-nutted hands to large raises.

2\. \*\*Bot 2 - "Loose" (Very Loose):\*\* Plays top 55% preflop hands. Calls postflop when `Equity >= PotOdds - 0.12` or holding any pair/draw; bluffs frequently (`BluffFreq = 0.35`).

3\. \*\*Bot 3 - "GTO" (Pure Baseline):\*\* Uses 6-max positional preflop lookup charts (UTG, HJ, CO, BTN, SB, BB). Postflop runs 5,000 Monte Carlo rollouts to estimate equity, compares against Pot Odds and Minimum Defense Frequency (`MDF = Pot / (Pot + Bet)`), sizes bets geometrically (33% dry, 75% wet, 125% polarized), and balances bluffs with draw equity.

4\. \*\*Bot 4 - "WildGTO" (GTO + Stochastically Uncautious):\*\* Computes the exact Bot 3 (GTO) move first. Rolls a random float `0.0 - 1.0`; with 20% probability (`p = 0.20`), overrides the GTO move with an uncautious aggressive or loose action (e.g., 2x pot overbet bluff, wide cold-call, or light hero-call).

5\. \*\*Bot 5 - "Shifter" (Random Chameleon):\*\* At the start of each hand, randomly selects one of `\[Nit, Loose, GTO, WildGTO]` as its active internal profile. Do not reveal the active sub-profile in the main table UI, but log it in the hand history as `"Shift:<SubProfile>"`.



\## 5. Real-Time GTO Training Hints Module

Create a `TrainingAdvisor` class that analyzes Hero's current decision state whenever it is Hero's turn:

\- Compute Hero's raw equity via Monte Carlo simulation (5,000 rollouts), exact Pot Odds %, Stack-to-Pot Ratio (SPR), and estimated EV for Fold, Check/Call, and Raise (25%, 50%, 75%, 100%, All-in). A heuristic EV decides whether to bet; a rule over board texture and hand strength chooses the size.

\- Output a structured `HintRecommendation` containing:

&#x20; - `best\_action`: e.g., "RAISE to 150 (75% Pot)"

&#x20; - `equity\_pct`, `pot\_odds\_pct`, `mdf\_pct`

&#x20; - `rationale`: A concise 1-2 sentence explanation referencing position, preflop range tier, board texture (wet/dry/paired/monotone), and equity vs. pot odds.

\- Render this in a toggleable "GTO Training Hints" ImGui window.



\## 6. Compact AI-Ready Hand History Logger

Implement `HandLogger` writing one single-line JSON object per completed hand to `logs/hand\_history.jsonl`:

\- Schema must be compact for LLM token efficiency:

&#x20; - `h` (hand number), `sb`, `bb`, `btn` (seat index).

&#x20; - `seats`: array of `{p, pos, st, c}` (include hole cards for all players who saw a flop or went to showdown, plus Bot 5's active sub-persona).

&#x20; - `pre`, `flop`, `turn`, `riv`: board cards, pot size entering the street, and compact action strings (`"POS:ActionAmount"`, where actions are `F`, `X`, `C`, `B`, `R`, `A`).

&#x20; - `win`: array of `{p, amt, hand}`.

&#x20; - `hero\_mistakes`: array of streets where Hero's chosen action deviated significantly from the GTO Hint recommendation.



\## 7. Phased Implementation Plan (Execute Step-by-Step)

\- \*\*Phase 1:\*\* Scaffold CMake project, Card/Deck structs, and 7-card hand evaluator with unit tests.

\- \*\*Phase 2:\*\* Build the headless NLHE `GameEngine` state machine (blinds, streets, legal action masks, side pots, freezeout tracking) and verify via CLI test harness.

\- \*\*Phase 3:\*\* Implement the 6-max preflop positional charts, Monte Carlo equity calculator, `TrainingAdvisor`, and the 5 Bot Personalities.

\- \*\*Phase 4:\*\* Implement the compact JSONL `HandLogger`.

\- \*\*Phase 5:\*\* Build the `Dear ImGui` desktop frontend connecting to `GameEngine` via a clean `IGameController` interface.

## 8. Current Status

All five phases are complete, and v1.0.0 has been released. Later work added the hand name, Play GTO, Fold Early, Reveal Bot Hands, bet percentages, and the 25/50/75/Pot sizing ladder. Sections 1-7 are the original specification, except where section 5 was updated for the ladder; the rest of this file records how it was built.

## 9. Build and Test

Uses the "Visual Studio 18 2026" generator, x64, in `build/`:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -j 8
```

- Finished work must have zero warnings and all tests passing in both Release and Debug. Tests use `gtest_discover_tests` with `PRE_TEST`, so each config runs its own binary.
- Linking fails with `LNK1104` while `poker_trainer.exe` is open. Don't close a running copy that may be the user's: verify in a temporary short-path folder such as `build/v` and delete it afterward. Long paths, such as the scratchpad, can make configuring fail.
- Sanitizers (Linux, GCC): configure a separate build with `-DPOKER_BUILD_APP=OFF`, `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and `CMAKE_CXX_FLAGS`/`CMAKE_C_FLAGS`/`CMAKE_EXE_LINKER_FLAGS` set to `-fsanitize=thread`, or to `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`, then run its `ctest`. Timing tests such as `SolverService.NewDecisionCancelsQuickly` can miss their bounds under TSan when the machine is busy; rerun them alone.
- Windowless UI tests are in `tests/trainer_ui_test.cpp`. They inject ImGui input at fixed coordinates, so layout changes mean updating those constants.
- `main.cpp` has developer flags for reaching rare screens: `--seed`, `--log-dir`, `--exit-after-frames`, `--auto-shove`, `--stop-on live-side-pot|side-pot|showdown|flop|bust`, `--reveal`, `--bot-delay MS`, `--solver`, `--solver-budget MS`, `--solver-tier off|river|turn|flop`.
- More developer flags for reviewing the UI: `--advice baseline|nit|station|maniac` (initial "Advice:" choice), `--hover X,Y` (holds the ImGui mouse there, for tooltips), `--size WxH` (window size, at least 1180x760), and `--screenshot PATH` (saves the last frame as a PNG, with `--exit-after-frames` or on close; works under `xvfb-run` on Linux).
- To screenshot the app when other windows may cover it, capture the window itself with Win32 `PrintWindow` (flag 2). `CopyFromScreen` captures whatever is on top.
- In PowerShell, double quotes inside a `git commit -m` here-string break the argument. Write the message to a file and use `git commit -F`.
- Restoring a file with `Copy-Item` keeps its old timestamp, so MSBuild may not rebuild it. Touch the file after restoring.

## 10. Architecture Rules

- **Hidden information:** bots and `TrainingAdvisor` receive only `PlayerView` (`src/engine/player_view`), never `GameState`. A CTest check, `no_engine_access_in_players`, enforces this.
- **Front end:** `TrainerUi` sees only `IGameController` and `TableSnapshot`. The snapshot has hidden information removed already, and new UI data should be added to it. The UI must not reach into engine state.
- **Shifter:** its active profile appears only in the hand log, as `Shift:<SubProfile>`, never in the snapshot or UI.
- **Logging:** a change to the UI or advice must not change the JSONL schema unless that's the request. `describe_hand` keeps log text stable; `describe_made_hand` gives UI names.

## 11. Decisions to Preserve

- **Rules:** TDA-style short all-ins (a short all-in doesn't reopen betting for players who have already acted), a moving button with no dead button, and no burn cards.
- **Pot sizing:** percentages use the pot after calling, in both `quick_bet_to` and `raise_to_for_pot_fraction`. `bet_pot_percent` is the inverse.
- **Finished pot:** after a hand, the pot shown is what was paid out, excluding returned uncalled chips.
- **Advisor is heuristic:** it isn't solver output, and the UI and docs must say so. Don't present its numbers as exact.
- **Advisor sizing:** the EV formula can't choose between sizes, because it collapses to the smallest or largest. Postflop, the model decides whether to bet, and `preferred_size` chooses the size. Preflop stays with charts and EV. See `training_advisor.h`.
- **`ev` and `base_ev`:** `ev` includes the fit adjustment, so the advice is the highest listed option. `base_ev` is the model alone, and hero mistakes are judged on it, with a 1 BB threshold.
- **Size rule limits:** fit must never turn a check, call or fold into a bet. All-in is chosen only within 1.5 pots, or when it's the only raise left.
- **GTO bot:** keeps 33%, 75% and 125% per section 4, so it intentionally differs from the advisor's ladder.
- **Quick-bet buttons:** each one sets the slider, while the action buttons act. Percentage buttons are capped at what an opponent can match, but the All-In quick button still goes to the full stack.
- **Fold Early:** the fold happens when action reaches Hero, through the normal action path, so bots can't react to it. A free check cancels it. Bots skip their delay only while armed.
- **Reveal Bot Hands:** a display option for finished hands only. It doesn't change logs, `shown_hand` or payouts.

## 12. Known Limits

- Real mouse and keyboard input through GLFW has not been tested end to end, only injected ImGui input and developer autoplay.
- All-in runouts deal the remaining board at once, not street by street.
- Fold Early has effectively no cancel window, because bots finish acting within a frame of arming it.
- Advisor equity is against random hands, and fold equity can recommend small bets with weak hands, such as 24% equity on a paired board.
- The sizing distribution is measured only for heads-up spots with no bet to face.
- New Session has no confirmation, and quitting mid-hand can make the summary's `hands` count one higher than the logged hands.
- Sanitizers have run only on Linux with GCC: ThreadSanitizer, and AddressSanitizer with UBSan, over the whole core test suite and a threaded turn-bench solve, with nothing reported. MSVC builds have not been sanitized.

## 13. Working Preferences

- Commit or push only when asked. Plans often say to commit locally and not push.
- Work directly on `main`. Don't create feature branches or pull requests, even if a session starts on another branch: switch to `main` first and push there.
- When asked to analyze or plan, don't implement until told to go ahead.
- Check poker math with real numbers before changing it, and report anything not verified.
- Release tags should use `vX.Y.Z`. The first release's tag is `v.1.0.0`, with an extra dot, and `project(VERSION)` in `CMakeLists.txt` still says 0.1.0.

