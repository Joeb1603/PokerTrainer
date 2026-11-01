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

\- Compute Hero's raw equity via Monte Carlo simulation (5,000 rollouts), exact Pot Odds %, Stack-to-Pot Ratio (SPR), and estimated EV for Fold, Check/Call, and Raise (33%, 75%, All-in).

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

