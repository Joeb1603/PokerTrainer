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

- *Note (game modes):* the freezeout above is still the default, now with its big blind and starting stack (in BB) chosen on a "New Game" screen. A second mode, Tournament, keeps the six seats and no rebuys but raises the blinds every N hands or minutes, from a preset schedule (Standard, Turbo, Hyper) that can be multiplied or edited level by level, with optional antes (per player or a big-blind ante); see section 8.



\## 4. Bot Architecture \& 5 Personalities

Implement an `IBotStrategy` interface driven by a shared `GtoHeuristicEngine` and parameterized `PersonalityProfile`:

1\. \*\*Bot 1 - "Nit" (Overly Cautious):\*\* Plays top 12% preflop hands. Postflop requires `Equity >= PotOdds + 0.15` to call; rarely bluffs (`BluffFreq = 0.03`); folds non-nutted hands to large raises.

2\. \*\*Bot 2 - "Loose" (Very Loose):\*\* Plays top 55% preflop hands. Calls postflop when `Equity >= PotOdds - 0.12` or holding any pair/draw; bluffs frequently (`BluffFreq = 0.35`).

3\. \*\*Bot 3 - "GTO" (Pure Baseline):\*\* Uses 6-max positional preflop lookup charts (UTG, HJ, CO, BTN, SB, BB). Postflop runs 5,000 Monte Carlo rollouts to estimate equity, compares against Pot Odds and Minimum Defense Frequency (`MDF = Pot / (Pot + Bet)`), sizes bets geometrically (33% dry, 75% wet, 125% polarized), and balances bluffs with draw equity.

4\. \*\*Bot 4 - "WildGTO" (GTO + Stochastically Uncautious):\*\* Computes the exact Bot 3 (GTO) move first. Rolls a random float `0.0 - 1.0`; with 20% probability (`p = 0.20`), overrides the GTO move with an uncautious aggressive or loose action (e.g., 2x pot overbet bluff, wide cold-call, or light hero-call).

5\. \*\*Bot 5 - "Shifter" (Random Chameleon):\*\* At the start of each hand, randomly selects one of `\[Nit, Loose, GTO, WildGTO]` as its active internal profile. Do not reveal the active sub-profile in the main table UI, but log it in the hand history as `"Shift:<SubProfile>"`.

- *Note (random personalities):* these five are still the default table. An opt-in "Random personalities" mode draws five distinct styles from a pool of 13 (these five plus TAG, LAG, Station, Maniac, Rock, Trapper, Bully and Tilter), with random names and places; see section 8.



\## 5. Real-Time GTO Training Hints Module

Create a `TrainingAdvisor` class that analyzes Hero's current decision state whenever it is Hero's turn:

\- Compute Hero's raw equity via Monte Carlo simulation (5,000 rollouts), exact Pot Odds %, Stack-to-Pot Ratio (SPR), and estimated EV for Fold, Check/Call, and Raise (25%, 50%, 75%, 100%, All-in). A heuristic EV decides whether to bet; a rule over board texture and hand strength chooses the size.

\- Output a structured `HintRecommendation` containing:

&#x20; - `best\_action`: e.g., "RAISE to 150 (75% Pot)"

&#x20; - `equity\_pct`, `pot\_odds\_pct`, `mdf\_pct`

&#x20; - `rationale`: A concise 1-2 sentence explanation referencing position, preflop range tier, board texture (wet/dry/paired/monotone), and equity vs. pot odds.

\- Render this in a toggleable "GTO Training Hints" ImGui window.

- *Note (advisor upgrade):* `equity_pct` is still raw equity against random hands from 5,000 Monte Carlo rollouts, as specified, and it feeds the log's `eq`. The advice now comes from equity against inferred opponent ranges (`range_equity_pct`), which the UI shows first. See section 8.



\## 6. Compact AI-Ready Hand History Logger

Implement `HandLogger` writing one single-line JSON object per completed hand to `logs/hand\_history.jsonl`:

\- Schema must be compact for LLM token efficiency:

&#x20; - `h` (hand number), `sb`, `bb`, `btn` (seat index).

&#x20; - `seats`: array of `{p, pos, st, c}` (include hole cards for all players who saw a flop or went to showdown, plus Bot 5's active sub-persona).

&#x20; - `pre`, `flop`, `turn`, `riv`: board cards, pot size entering the street, and compact action strings (`"POS:ActionAmount"`, where actions are `F`, `X`, `C`, `B`, `R`, `A`).

&#x20; - `win`: array of `{p, amt, hand}`.

&#x20; - `hero\_mistakes`: array of streets where Hero's chosen action deviated significantly from the GTO Hint recommendation.

- *Note (game modes):* `sb` and `bb` are now each hand's own blinds. Tournaments add `lvl`, hands with antes add `ante`, `at` and `post.ante`, and the session line adds `mode`, `lvl`, `place` and `sched`; a freezeout without antes writes exactly the old lines. See `hand_logger.h`.



\## 7. Phased Implementation Plan (Execute Step-by-Step)

\- \*\*Phase 1:\*\* Scaffold CMake project, Card/Deck structs, and 7-card hand evaluator with unit tests.

\- \*\*Phase 2:\*\* Build the headless NLHE `GameEngine` state machine (blinds, streets, legal action masks, side pots, freezeout tracking) and verify via CLI test harness.

\- \*\*Phase 3:\*\* Implement the 6-max preflop positional charts, Monte Carlo equity calculator, `TrainingAdvisor`, and the 5 Bot Personalities.

\- \*\*Phase 4:\*\* Implement the compact JSONL `HandLogger`.

\- \*\*Phase 5:\*\* Build the `Dear ImGui` desktop frontend connecting to `GameEngine` via a clean `IGameController` interface.

## 8. Current Status

All five phases are complete. Releases: `v.1.0.0` (the initial release); `v1.1.0` (the hand name, Play GTO, Fold Early, Reveal Bot Hands, bet percentages, and the 25/50/75/Pot sizing ladder); and `v1.2.0` (the advisor upgrade below, built and published by the release workflow in section 9). Sections 1-7 are the original specification, except where section 5 was updated for the ladder and given a note on range equity; the rest of this file records how it was built.

The advisor upgrade (range-based advice, player-type advice, solver mode) is done. `docs/advisor-upgrade-notes.md` is its decision log: every decision, deviation, measurement, unverified item and test change. Read it before changing the advisor, range model or solver. What it added:
- **Range model** (`src/range`): combos, weighted ranges, exact per-combo equity vectors on every postflop street, a 169×169 preflop class matrix, and range inference from public actions for four generic player types: Baseline, Nit, Station, Maniac (`range_model.h`).
- **Range-based EV:** check, call and bet/raise EVs use equity against the inferred ranges and each villain's predicted fold, call or raise share. Players still to act are weighted by their predicted continue. `hint.versus` holds one advice line per type (Nit, Station, Maniac), with a generated `why`.
- **Preflop:** positional answers to 3-bets and 4-bets, advisor-only vs-open (defence) charts (`advisor_vs_open_ranges`, `advisor_chart`), and push/fold tables for 3-15 BB (`src/strategy/push_fold`). The bots' charts are unchanged.
- **Solver:** our own Discounted CFR (`src/solver`), heads-up postflop, with river, turn and flop (full or approximate) trees, memory estimates before allocation, and a thread pool. Solver mode runs it in the background (`src/controller/solver_service`) on a tier chosen from the machine's threads and RAM (`solver/hardware.h`).
- **UI:** an "Advice:" selector (Baseline/Nit/Station/Maniac) with tooltips, a "Versus types" table, and a Solver mode checkbox with budget slider, progress and frequencies.
- **Benches** (`tools/bench`): 36 river and 12 turn spots, scored against the solver. River regret fell from 1.946 to 0.694 BB per decision, and agreement with the solver's top action rose from 28% to 57%. On the turn holdout the tuned Baseline gives 0.543 BB and 33%, against 0.614 BB and 39% with the pre-tuning parameters.

Random personalities (after `v1.2.0`, not yet released) is done. With the option off, the classic table is byte-identical. What it added:
- **Knobs** in `PersonalityProfile` (`gto_heuristic_engine.h`): `preflop_sizing`, `bet_sizes`, `positional_width` (TopPercent cut-offs by position), `slowplay_freq` and `cbet_freq`. Defaults reproduce the classic behaviour, and a knob at 0 never draws from the generator.
- **Archetypes:** `tag_profile`, `lag_profile`, `station_profile`, `maniac_profile`, `rock_profile`, `trapper_profile`, `bully_profile`, and `TiltBot` ("Tilter": TAG, then Maniac for `kTiltHands` hands after losing `kTiltLossBigBlinds`). `BotKind` gains them at the end; `kAllBotKinds` is still the classic lineup, `kExtraBotKinds` the new eight, `kRandomPoolKinds` all 13; `draw_random_lineup` picks five distinct kinds in seat order.
- **Controller:** `ControllerOptions::random_personalities` and `IGameController::set_random_personalities`, applied at the next `new_session()`. Names come from `controller/bot_names`. The lineup has its own stream (`seed ^ kLineupStream`).
- **Logs:** with the option on, `p` is `Name:Style` (`Marta:LAG`, `Marta:Shift:Nit`, `Marta:Tilt:On`); session summaries use the public style (`Marta:Shifter`). The schema is unchanged.
- **UI:** a "Random personalities" checkbox under the Advice selector ("(next session)" until it applies), the lineup on the session-over console, and Hero's own folded cards kept on the table, dimmed, for the rest of the hand.
- **App:** `poker_trainer_settings.json` beside `logs/` remembers the option (`src/app/app_settings`).
- **Pins:** `Golden.ClassicBotsDecideAsBefore` and `Controller.OffModeLogsUnchanged` hash the classic bots' decisions and the default table's logs; `TableReport.DISABLED_PoolStats` prints VPIP, PFR and aggression per style.

Game modes (after `v1.2.0`, not yet released) is done. With the default setup (Freezeout, 10/20, 100 BB, no antes) the table and its logs are byte-identical. What it added:
- **Engine:** `BlindLevel` and `AnteType` (`table_types.h`); `GameState::level` holds each hand's blinds and ante; `set_level()` between hands; `new_game(GameConfig)` swaps stacks and blinds but keeps the RNG running. Antes are dead money outside `total_bet` (`Seat::ante_posted`, `PublicSeat::ante_posted`, `PlayerView::dead_money()`): per-player antes layer into side pots, a big-blind ante goes to the main pot (`calculate_pots(..., dead_money)`); neither changes the bet to call, the minimum raise or the uncalled refund.
- **Schedule** (`src/controller/blind_schedule`): `GameSetup`, presets as BB multiples rounded by `tidy_chips`, `with_antes`, `scale_schedule`, `validate`, and `TournamentClock` (levels by hands or by `tick()` time, changing only when a hand starts; Pause; each tick counts at most 1 s).
- **Controller:** `ControllerOptions::setup` replaces `starting_stack`/`small_blind`/`big_blind`; `set_game_setup` applies at the next `new_session()`, like random personalities. The snapshot has the hand's blinds and ante, `mode`, `tournament` (`TournamentPanel`) and `setup_pending`; `SessionStats` has `mode`, `level_reached` and `hero_place`.
- **Player side:** pot odds and pot layers count the dead money (`pot_math`, `pot_layers`); push/fold has ante tables at 0.5, 1 and 1.5 BB of dead money (`push_fold_ante_data.cpp`, chosen as the largest not above the pot's), used by the advisor and range model. The bots' charts and the advisor's opening charts are unchanged.
- **UI:** a "New Game" screen (`src/app/setup_panel`) at launch and from New Session and Start New Game; the level, ante and next level in the felt's top-left corner, with Pause clock in minutes mode; the finishing place and level on the session-over console.
- **App:** the settings file also keeps `game_setup`, saved when Start is pressed; game flags in section 9.
- **Tools:** `advisor_matchup --stack-bb N --antes none|player|bb` plays short, ante hands.

## 9. Build and Test

Uses the "Visual Studio 18 2026" generator, x64, in `build/`:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -j 8
```

- Finished work must have zero warnings and all tests passing in both Release and Debug. Tests use `gtest_discover_tests` with `PRE_TEST`, so each config runs its own binary.
- Linking fails with `LNK1104` while `poker_trainer.exe` is open. Don't close a running copy that may be the user's: verify in a temporary short-path folder such as `build/v` and delete it afterward. Long paths, such as the scratchpad, can make configuring fail.
- **Linux** (cloud containers; not the release target): install `libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev xvfb`, configure with `-DGLFW_BUILD_WAYLAND=OFF` (GLFW fails without it) and `-DCMAKE_BUILD_TYPE=Release` or `Debug`, keep build folders outside the repo (e.g. `/tmp/claude-0/b/Release`), and run tests with `xvfb-run -a ctest --test-dir <dir> --output-on-failure -j 4`, because the UI tests need a display. The suite takes about 9 s in Release and 35 s in Debug on 4 cores.
- **MSVC stand-in:** MSVC can't run on Linux, so new or changed files are also checked with `clang++ -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wfloat-conversion -Wsign-compare` (include `src` and the `_deps` headers as `-isystem`), counting only warnings in the files checked. It approximates `/W4`; it doesn't replace a real MSVC build.
- **Tools** (built with the project; `tools/` isn't covered by the hidden-information scan, so tools may use `GameEngine`):
  - `advisor_bench`: scores the advisor against the solver. `--spots tools/bench/turn_spots.json` for the turn (river is the default), `--stability`, `--timing`, `--threads`, `--only ID`, `--csv`, `--baseline-params C,V,B`. A river run takes about 10 s, a turn run about 70 s. Spots and flags: `tools/bench/README.md`.
  - `gen_river_spots.py` and `gen_turn_spots.py` (in `tools/bench`) write the spot files.
  - `solver_cli`: runs one solve (`--board`, `--pot`, `--stack`, `--oop`/`--ip` ranges, `--tier river|turn|flop|flop-checkdown`, `--threads`, `--memory-mb`).
  - `gen_preflop_tables` (about 200 s on 4 threads) and `gen_push_fold` (about 3 s) regenerate the committed tables in `src/range/generated` and `src/strategy/generated`, byte for byte.
  - `tools/advisor_matchup`: plays each player-type line against the bots, paired with Baseline, and reports bb/100.
- Sanitizers (Linux, GCC): configure a separate build with `-DPOKER_BUILD_APP=OFF`, `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and `CMAKE_CXX_FLAGS`/`CMAKE_C_FLAGS`/`CMAKE_EXE_LINKER_FLAGS` set to `-fsanitize=thread`, or to `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`, then run its `ctest`. Timing tests such as `SolverService.NewDecisionCancelsQuickly` can miss their bounds under TSan when the machine is busy; rerun them alone.
- **Releases:** `.github/workflows/release.yml` builds and publishes them. It runs only by hand, from the Actions tab: "Windows release" > "Run workflow", with a tag, a title and a "publish" switch.
  - It builds with MSVC on `windows-latest` (Visual Studio 2022, x64) in Release and Debug.
  - It fails on any compiler or linker warning outside `_deps` (`.github/scripts/check-warnings.ps1`).
  - It runs the full ctest suite in both configurations.
  - With "publish" on, it creates the tag and the release from `.github/release-notes/<tag>.md`, with `poker_trainer.exe` attached.
  - To release: write and push the notes file first, then run once with "publish" off to check, then again with it on.
  - The GitHub tools available to Claude can't create a release directly, but they can trigger this workflow and read its logs.
- Windowless UI tests are in `tests/trainer_ui_test.cpp`. They inject ImGui input at fixed coordinates, so layout changes mean updating those constants.
- `main.cpp` has developer flags for reaching rare screens: `--seed`, `--log-dir`, `--exit-after-frames`, `--auto-shove`, `--stop-on live-side-pot|side-pot|showdown|flop|bust|folded`, `--reveal`, `--bot-delay MS`, `--solver`, `--solver-budget MS`, `--solver-tier off|river|turn|flop`. `folded` has Hero fold a hand that goes on, then pauses the bots.
- User-facing flags: `--random-personalities on|off` (sets the option for one run without saving it) and `--settings PATH` (the settings file; default `poker_trainer_settings.json` in the working folder). Pass `--settings` when running the app from a tool, so a run never writes into the repo.
- Game flags, for one run without saving: `--mode freezeout|tournament`, `--big-blind N`, `--stack-bb N`, `--level-hands N` or `--level-minutes N`, `--preset standard|turbo|hyper`, `--antes none|player|bb`. The New Game screen opens first unless `--skip-setup`, `--exit-after-frames`, `--auto-shove`, `--stop-on` or `--screenshot` is given; `--show-setup` opens it anyway (for screenshots of it). Closing the window on it quits without starting a session. A flag value that `validate()` rejects is shown there as the problem, or stops the app when the screen is skipped. The settings file changes only when the player presses Start (the whole screen) or toggles the sidebar checkbox (that option alone).
- More developer flags for reviewing the UI: `--advice baseline|nit|station|maniac` (initial "Advice:" choice), `--hover X,Y` (holds the ImGui mouse there, for tooltips), `--size WxH` (window size, at least 1180x760), and `--screenshot PATH` (saves the last frame as a PNG, with `--exit-after-frames` or on close; works under `xvfb-run` on Linux).
- To screenshot the app when other windows may cover it, capture the window itself with Win32 `PrintWindow` (flag 2). `CopyFromScreen` captures whatever is on top.
- In PowerShell, double quotes inside a `git commit -m` here-string break the argument. Write the message to a file and use `git commit -F`.
- Restoring a file with `Copy-Item` keeps its old timestamp, so MSBuild may not rebuild it. Touch the file after restoring.

## 10. Architecture Rules

- **Hidden information:** bots and `TrainingAdvisor` receive only `PlayerView` (`src/engine/player_view`), never `GameState`. A CTest check, `no_engine_access_in_players`, enforces this.
- **Player-side code:** `src/range`, `src/solver` and `src/controller/solver_service.*` count as player-side, like `src/bots`, `src/advisor`, `src/strategy` and `src/equity`. The scan covers them all and forbids the word `Seat`, `.hole`, `last_result` and `showdown_ranks`, comments included. Their headers are also compiled in `tests/player_boundary_test.cpp`.
- **Solver service:** it gets only a `PlayerView` and builds both ranges from it with the range model (every villain read as Baseline). Its advice covers Hero's own combo only.
- **Player types are generic:** Baseline, Nit, Station and Maniac are applied to all opponents alike. No per-seat type, label or bot identity may reach the advisor, the solver or the UI, so `PlayerView` stays unchanged and the Shifter stays hidden.
- **Random personalities:** bot names come from the controller and say nothing about style. A bot's style reaches only the hand log and, once the session is over, `SessionStats::lineup` (a deliberate exception the user asked for: the reveal at the end). The Shifter shows there as "Shifter", never its sub-profile. Nothing about the lineup reaches the advisor or the solver.
- **Game setup:** the New Game screen (`SetupPanel`) edits only a `GameSetup`; `TrainerUi` applies it through `IGameController::set_game_setup` and `new_session()`. The tournament clock lives in the controller; the app passes no time to `tick()` while the screen is open.
- **Front end:** `TrainerUi` sees only `IGameController` and `TableSnapshot`. The snapshot has hidden information removed already, and new UI data should be added to it. The UI must not reach into engine state.
- **Shifter and Tilter:** the Shifter's active profile appears only in the hand log, as `Shift:<SubProfile>`, never in the snapshot or UI; so does the Tilter's state, as `Tilt:On` or `Tilt:Off`.
- **Logging:** a change to the UI or advice must not change the JSONL schema unless that's the request. `describe_hand` keeps log text stable; `describe_made_hand` gives UI names.

## 11. Decisions to Preserve

- **Rules:** TDA-style short all-ins (a short all-in doesn't reopen betting for players who have already acted), a moving button with no dead button, and no burn cards.
- **Pot sizing:** percentages use the pot after calling, in both `quick_bet_to` and `raise_to_for_pot_fraction`. `bet_pot_percent` is the inverse.
- **Finished pot:** after a hand, the pot shown is what was paid out, excluding returned uncalled chips.
- **Advisor is heuristic:** heuristic estimates against assumed ranges, not solver output, and the UI and docs must say so. Don't present its numbers as exact. Solver output is labelled approximate too ("Approximate solver: simplified bet sizes, assumed ranges."), including in the heading.
- **Advisor sizing:** the EV formula can't choose between sizes, because it collapses to the smallest or largest. Postflop, the range EV decides whether to bet, and `preferred_size` chooses the size. Preflop, `advisor_chart` and EV choose. See `training_advisor.h`.
- **`preferred_size` keeps random-hand equity** as its input, because its strength bars were calibrated on it.
- **`equity_pct` stays raw:** random-hand Monte Carlo, 5,000 rollouts, logged as `eq`. `range_equity_pct` is added beside it and shown first.
- **`ev` and `base_ev`:** `ev` includes the fit adjustment, so the advice is the highest listed option. `base_ev` is the model alone, and hero mistakes are judged on it, with a 1 BB threshold.
- **Baseline is the judge:** the snapshot's `hint` is the Baseline advice, with the type lines in `hint.versus`. Mistakes and the JSONL are judged on Baseline `base_ev` only, whatever "Advice:" shows and whether solver mode is on. Solver mode must leave the logs byte-identical (`Controller.SolverDoesNotChangeMistakesOrLogs`).
- **Size rule limits:** fit must never turn a check, call or fold into a bet. All-in is chosen only within 1.5 pots, or when it's the only raise left. Preflop, all-in is also advised with an effective stack of 15 BB or less (push/fold).
- **Bots stay on random-hand equity:** per section 4, with `bet_pressure`, and they use the bots' charts (`chart_response`, `vs_open_ranges`), which `VsReraises.BotsChartsUnchanged` pins. The advisor's positional and vs-open charts are advisor-only. The bots are also the fixed testbed for the type lines.
- **`implied` is 0 preflop:** it made the smallest or largest size win; the chart prior stands in for later streets.
- **Baseline parameters** (continue_mult 0.85, value_bet_top 0.28, bluff_mult 1.2) were tuned once on the river bench, with the turn bench as holdout. Don't retune on the same bench without a new holdout.
- **Play GTO** plays the selected "Advice:" line. It plays the solver's most frequent action (ties to the cheaper) only when the solve is Done, Advice is Baseline, the advisor is shown and the exploitability is within 3× the tree's target (`solve_is_usable`).
- **Solver scope:** heads-up postflop only; preflop, multiway and an all-in villain fall back to the heuristic with a note. It is approximate: abstracted bet sizes (the advisor's ladder at the root only), Baseline ranges and the average strategy when the budget ends. The full flop tree needs both the memory budget and a budget of at least 120 s (`kFlopFullMinBudgetMs`); otherwise the flop uses the approximate tree with no river betting, always labelled so.
- **Determinism:** new code uses no `std::uniform_real_distribution` or `std::shuffle` (use raw `rng()` bits and `Deck::shuffle`), integer tallies, fixed-order sums and ties broken by combo index. Results must not depend on the thread count.
- **Solver results are keyed** by `{decision_epoch, hand_number, action_count}`; the epoch is bumped for every Hero decision, because `new_session()` restarts hand numbers.
- **GTO bot:** keeps 33%, 75% and 125% per section 4, so it intentionally differs from the advisor's ladder.
- **Quick-bet buttons:** each one sets the slider, while the action buttons act. Percentage buttons are capped at what an opponent can match, but the All-In quick button still goes to the full stack.
- **Fold Early:** the fold happens when action reaches Hero, through the normal action path, so bots can't react to it. A free check cancels it. Bots skip their delay only while armed.
- **Reveal Bot Hands:** a display option for finished hands only. It doesn't change logs, `shown_hand` or payouts. Hero's own folded cards don't wait for it: they stay on the table, dimmed (`revealed_after_fold`), for the rest of the hand.
- **Random personalities off is the classic table, byte-identical:** the classic bots are seeded `seed + 1000 + seat` and kept across `new_session()`, as before; only a random lineup (or leaving one) rebuilds them. New knobs draw from the generator only when above 0, through raw bits (`unit_roll`, `uniform_below` in `core/random.h`). `Golden.ClassicBotsDecideAsBefore` and `Controller.OffModeLogsUnchanged` pin this.
- **The lineup has its own stream** (`seed ^ kLineupStream`): five kinds, five names, then one seed per bot, in that order.
- **The default game is the classic table, byte-identical:** Freezeout, 10/20, 100 BB, no antes. New log fields appear only in tournaments or hands with antes. The engine is never rebuilt: `new_game(GameConfig)` keeps its RNG, so a new setup never replays earlier deals.
- **Antes are dead money outside `total_bet`:** player code that needs them uses `ante_posted` or `PlayerView::dead_money()`. The posting order is per-player antes, then the blinds, then a big-blind ante from what the big blind has left (TDA). The blind seats are chosen before any ante, so heads-up detection is unchanged.
- **Levels change only when a hand starts**, computed from hands started or time played, so a pause can skip levels; the last level lasts for ever.

## 12. Known Limits

- Real mouse and keyboard input through GLFW has not been tested end to end, only injected ImGui input and developer autoplay.
- All-in runouts deal the remaining board at once, not street by street.
- Fold Early has effectively no cancel window, because bots finish acting within a frame of arming it.
- The sizing distribution is measured only for heads-up spots with no bet to face.
- New Session opens the New Game screen, whose Cancel keeps the game going; Start ends the game even mid-hand. Quitting mid-hand can make the summary's `hands` count one higher than the logged hands.
- **Random personalities:** the archetypes' numbers are judgement, checked only for being distinct (`TableReport.DISABLED_PoolStats`), not tuned or benchmarked against the solver. The Tilter measures its loss between hands it acts in, so a walk in the big blind folds into the next comparison. The golden hashes are pinned for libstdc++ only (the classic bots draw through library-defined distributions), so on MSVC the tests print the hash and skip. Saving the setting from a real checkbox click has been checked only through the injected-input UI test and the settings tests, not end to end.
- **Windows:** the release workflow builds the upgrade with MSVC `/W4` with zero warnings in project code, and passes all 495 tests in Release and Debug on `windows-latest`.
  - Those tests also call `GlobalMemoryStatusEx`, in `Hardware.DetectReturnsSomethingSane`.
  - Not yet checked: running the app on a real Windows PC, Windows fonts and DPI scales other than 1 for the new UI, and the solver tiers on real Windows hardware.
  - The benchmarks, timings and screenshots all come from a 4-core Linux container (Mesa under Xvfb).
- Sanitizers have run only on Linux with GCC: ThreadSanitizer, and AddressSanitizer with UBSan, over the whole core test suite and a threaded turn-bench solve, with nothing reported. MSVC builds, clang's sanitizers and a Debug ASan build have not been run.

**Game modes**
- The presets and their ante rule (a big-blind ante of 1 BB, or BB/8 per player, from level 4) are judgement, not taken from any published structure.
- Antes reach the advisor and range model only through pot odds, pot layers and the push/fold ante tables. The advisor's opening and 3-bet charts, standard open sizes and the bots' charts assume no ante. A player all in from the ante alone contests only the antes; that layer is not modelled in pot odds.
- The push/fold ante tables are solved at 0.5, 1 and 1.5 BB of dead money (stacks after antes) and the largest not above the pot's is used, so ranges are slightly too tight between levels. Heads-up (where the one-caller model is exact) they were checked against an independent solve (phevaluator Monte Carlo equities, its own fictitious play) at 3-15 BB and every dead-money level: at most one borderline class apart (0.9 pp), pinned by `PushFoldAntes.HeadsUpMatchesAnIndependentSolve`. The only outside figure found, 71% shoves and 53% calls at 10 BB with a big-blind ante, matches the 1 BB table (71.0% and 53.2%), but its ante size could not be confirmed (the page could not be fetched). The multiway tables were not checked against any outside source.
- The minutes clock counts Hero's thinking time and finished-hand time; it stops only when paused, on the New Game screen and once the game is over. The Tilter counts antes paid as losses, and its loss threshold is in the current big blinds.
- Hero's place when several players bust on the same hand uses the chips each started that hand with; a tie goes to Hero.

**Range model and advice**
- Player types are generic archetypes applied to every opponent, not reads on any seat. Reading each seat's type from its stats is future work. The type lines were not checked against a solver; `tools/advisor_matchup` measures them only against the bots.
- Range weaknesses (`range_model.h`, log "Stage 4"): few pure bluffs after three barrels; the Station keeps a lot of air after calling two barrels; the Maniac bets almost its whole range at small sizes; a player who checks twice and then leads (e.g. a 3-bettor's check-check-lead on the river) is read as medium strength, which drives the worst bench spots (3-bet pots, facing 75%). Many constants (draw thresholds, the medium bet rate, `lead_share`, the 15-25 BB blend, the size exponents) are judgment, not fitted.
- The bench ranges are hand-written, and the tuned Baseline sits at a corner of a one-round grid. On the turn holdout tuning lowered regret but cut agreement with the solver by 6 pp. Facing turn bets the advisor raises and calls more, and folds less, than the solver; first to act it bets where the solver mostly checks.
- Fold equity can still recommend small stabs with weak hands (54o 25% pot on Kd8s3c: 24% folds).
- Preflop: the ladder's EVs are nearly flat, so min-3-bets sometimes win; premiums open 3.5 BB while most opens are 2-2.25 BB, a size tell.
- Multiway: players still to act continue independently; folders hold cards from their fold parts only in multiway scenarios; the original bettor never re-raises after a squeeze; an overcall of a short jam uses the lone-call push/fold range. None of this was checked against a multiway solve. Nearly impossible multiway deals cause latency spikes (about 100-300 ms) and fall back to independent opponents.
- Push/fold tables use a one-caller approximation (overcalls ignored, independent folds), are rounded from mixed strategies (up to 0.40 BB per hand for the worst classes) and are checked against published numbers only heads-up at 10 BB.

**Solver mode**
- Abstracted sizes, Baseline ranges only, and the average strategy when the budget ends. At the default 8 s, a wide heads-up spot stopped at 1.8% (turn) and 5.8% (approximate flop) of the pot. Hero's range is the public one, while the villain's has Hero's cards removed.
- The approximate flop tree checks the river down; its error against the full tree was not measured. The full flop tree needs a 120 s budget and several GB (3.3 GB on a 6-max spot, 5.2 GB with heads-up ranges, 7.7 GB with full ranges), and whether it beats the approximate tree at 120 s on 8+ threads is unmeasured. Nothing above 4 threads was tested.
- Container (cgroup) memory limits aren't read; other platforms than Windows and Linux fall back to the river tier untested.
- Budgets below one iteration plus setup overrun slightly; range inference, the tree build and river iterations can't be interrupted.
- Following the solver can still be flagged as a mistake, since mistakes are judged on Baseline. With the advisor hidden, solver mode keeps solving in the background.

## 13. Working Preferences

- Commit or push only when asked. Plans often say to commit locally and not push.
- Work directly on `main`. Don't create feature branches or pull requests, even if a session starts on another branch: switch to `main` first and push there.
- When asked to analyze or plan, don't implement until told to go ahead.
- **Plans** cover every task asked for, split large tasks into subtasks where that helps, and give each task or subtask an effort level. Usage isn't a concern, so set each level as high as will help.
  - Ask all questions while planning. Once the plan is approved, the user is hands off: finish every task without stopping to ask, unless a real blocker appears.
  - Choose how to run it: in one pass, one task at a time, or with an agent per task, whichever suits the work.
- Check poker math with real numbers before changing it, and report anything not verified.
- Release tags should use `vX.Y.Z`, as `v1.1.0` and `v1.2.0` do. The first release's tag is `v.1.0.0`, with an extra dot. `project(VERSION)` in `CMakeLists.txt` still says 0.1.0.

