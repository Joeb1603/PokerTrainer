# PokerTrainer

**Sharpen your 6-max No-Limit Hold'em against five very different opponents, with a training advisor at your side.**

PokerTrainer is a standalone Windows desktop app. You play a freezeout against five AI bots while the advisor breaks down the odds at every decision. It also flags your costliest mistakes and logs each hand in a format that's easy to review later.

## Download and play

1. Download `poker_trainer.exe` from the [latest release](https://github.com/Joeb1603/PokerTrainer/releases/latest).
2. Run it. There's nothing to install, and no extra runtimes are needed.
3. Play. Each game starts at 2,000 chips (100 BB) with 10/20 blinds and no re-buys.

Hand logs are saved to a `logs` folder inside the folder you launch the app from, so run it from somewhere you can write files.

The executable isn't code-signed, so Windows SmartScreen may warn you the first time you run it. Choose **More info**, then **Run anyway**.

**Requirements:** Windows and a graphics driver supporting OpenGL 3.0. Built and tested on Windows 11.

## Features

### The table
- 6-max table with Hero at the bottom center, rendered with Dear ImGui and GLFW
- 4-color deck, so suits are easy to tell apart at a glance
- Live main and side pot display, showing who is eligible for each pot
- Fold, check, call, bet, raise and all-in controls, with a raise slider and quick-bet sizes (Min, 25%, 50%, 75%, Pot, All-In) that match the advisor's sizes, and the bet shown as a percentage of the pot
- Your current hand named from the flop onward, such as `Two Pair, Aces and Kings`
- **Play GTO** button that plays the advice you have selected in one click: the Baseline advice, a player-type line, or a converged solve in solver mode
- **Fold Early** button that skips the wait: bots act without their delay, and you fold when action reaches you, unless you could check for free
- Adjustable bot speed or instant play, and a pause after each hand (press **Space** or click **Next Hand**)
- **Reveal Bot Hands** button at the end of a hand, including bots that folded

### The poker engine
- Fisher-Yates shuffle, implemented directly in the deck code
- Hand ranking powered by [phevaluator](https://github.com/HenryRLee/PokerHandEvaluator)
- TDA-style handling of short all-ins: a short all-in doesn't reopen betting for players who have already acted
- Multi-way side pot calculation and payouts
- Button rotation, heads-up blind rules, minimum raise sizing and freezeout bust-outs

### The bots

| Bot | Style |
|---|---|
| **Nit** | Plays about the top 12% of hands, needs a clear equity margin to continue and rarely bluffs |
| **Loose** | Plays about the top 55% of hands, calls with any pair or draw and bluffs often |
| **GTO** | The baseline: positional charts, Monte Carlo equity, Minimum Defense Frequency and board-based bet sizing |
| **WildGTO** | Starts from the GTO move, then overrides it 20% of the time with an aggressive or loose play |
| **Shifter** | Secretly becomes one of the other four at the start of each hand |

Bots can't cheat. They decide using a sanitized view of the game that contains no opponent hole cards.

### The training advisor
A toggleable panel that analyzes every decision you face:
- **The numbers:** your equity against the opponents' estimated ranges, with equity against a random hand (5,000 Monte Carlo rollouts) in its tooltip, plus pot odds, Minimum Defense Frequency (MDF) and Stack-to-Pot Ratio (SPR)
- **Estimated ranges:** each opponent's range is inferred from the public betting, preflop through the river, using positional charts, answers to 3-bets and 4-bets, and push/fold tables for stacks of 15 BB or less
- **The options:** estimated EV for folding, checking or calling, raising 25%, 50%, 75% or Pot, and all-in, with a tooltip on each bet or raise showing how often opponents are expected to fold or raise, and your equity when called
- **The recommendation:** a best action, such as `RAISE to 150 (75% Pot)`
- **The reasoning:** a short rationale covering position, preflop range tier, board texture and equity vs. pot odds

The advisor gives heuristic estimates against assumed ranges, not solver output. Its EV figures and "GTO" recommendations are training estimates, not true game-theory-optimal solutions. The EV model decides whether to bet, and a rule based on board texture and hand strength chooses the size.

### Advice against player types
The **Advice:** selector switches the panel between Baseline (a solid regular, the default) and three types: **Nit**, **Station** (a calling station) and **Maniac**. Each line answers the question "what if every opponent played like this?", with tooltips explaining the type and why the advice changes, such as bluffing a Nit more or value betting a station thinner and bigger. A **Versus types** table shows all three lines at once.

The types are generic and applied to every opponent; they are not reads on any particular bot. Mistakes are always judged against the Baseline advice.

### Solver mode
An optional **Solver mode** checkbox runs an equilibrium solver (Discounted CFR) in the background on heads-up postflop decisions and shows its action frequencies and EVs for your hand, with a progress bar and a time budget slider (2-30 s, or up to 120 s on the strongest machines). What it can solve depends on your machine:

| Tier | Needs | Solves |
|---|---|---|
| River | 2 threads, 1 GB free | river decisions |
| Turn | 4 threads, 4 GB free | turn and river, plus an approximate flop (no river betting) |
| Flop | 8 threads, 8 GB free | the full flop tree too, when it fits in memory and the budget is 120 s |

Preflop and multiway pots, and spots where your opponent is all-in, keep the heuristic advice with a note saying why. **Play GTO** follows the solver only once a solve is close enough to converged; otherwise it plays the heuristic advice.

It's an approximate solver: simplified bet sizes, assumed ranges, and a time limit. Treat its output as a guide, not the true equilibrium.

### Hand history and session logs
Both logs are JSONL, with one compact JSON object per line, so they're easy to analyze with scripts or paste into an LLM.

- `hand_history.jsonl` records every completed hand, including posted blinds, streets, actions and winners
- A separate session log summarizes each game, and a shared `session_id` links hands to their session
- When your choice falls at least 1 BB of estimated EV short of the advisor's recommendation, the hand's `hero_mistakes` entry records your equity, the pot odds, the recommended action and the rationale

## Known limitations

- When betting is closed by an all-in, the remaining board cards appear together rather than street by street.
- **New Session** resets immediately with no confirmation, and any hand in progress is discarded.
- If you quit mid-hand, the session's `hands` count can be one higher than the number of logged hands.
- The range-based advisor and solver mode were developed and tested on Linux; their Windows build and the solver's Windows memory detection haven't been checked yet.
- Solver mode's full flop solve needs a fast machine with lots of memory and a 120 s budget; shorter budgets use an approximate flop tree without river betting.

## Build from source

You'll need CMake 3.25 or newer and MSVC with C++20 support. Dependencies are fetched automatically.

```powershell
git clone https://github.com/Joeb1603/PokerTrainer.git
cd PokerTrainer
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This builds two programs:

- `build\Release\poker_trainer.exe`: the desktop app, which accepts `--seed N` for a repeatable game and `--log-dir DIR` to choose where logs go
- `build\Release\poker_cli.exe`: a headless test harness for the engine, with `--games N`, `--seed S` and `--verbose`

It also builds developer tools: `advisor_bench` (scores the advisor against the solver on the spots in `tools/bench`), `solver_cli` (runs a single solve), and the generators for the preflop equity and push/fold tables.

On Linux, install the X11 and OpenGL development packages (for example `libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`), configure with `-DGLFW_BUILD_WAYLAND=OFF -DCMAKE_BUILD_TYPE=Release`, and run the tests under `xvfb-run` if there's no display. Windows is the supported platform; Linux is used for development and testing.

## Project layout

The game logic knows nothing about the GUI. The app talks to it only through `IGameController`, so the front end could later be replaced, for example by a web server, without changing the engine.

| Path | Contents |
|---|---|
| `src/core` | Cards, deck and hand evaluation |
| `src/engine` | Game state machine, side pots and sanitized player views |
| `src/equity` | Monte Carlo equity calculator |
| `src/strategy` | Preflop positional charts and push/fold tables |
| `src/bots` | Shared heuristic engine and the five personalities |
| `src/range` | Combos, weighted ranges, range equity and the range model |
| `src/solver` | Discounted CFR postflop solver, betting trees and hardware tiers |
| `src/advisor` | Training advisor and its preflop charts |
| `src/logging` | Hand history and session loggers |
| `src/controller` | `IGameController`, the local game loop and the background solver service |
| `src/app` | Dear ImGui front end |
| `tools` | Command-line test harness, advisor bench, solver CLI and table generators |
| `docs` | Advisor upgrade decision log |
| `tests` | GoogleTest suite |

## Built with

[Dear ImGui](https://github.com/ocornut/imgui) · [GLFW](https://www.glfw.org/) · [phevaluator](https://github.com/HenryRLee/PokerHandEvaluator) · [nlohmann/json](https://github.com/nlohmann/json) · [GoogleTest](https://github.com/google/googletest)
