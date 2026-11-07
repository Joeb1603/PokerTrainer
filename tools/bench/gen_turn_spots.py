#!/usr/bin/env python3
# Builds tools/bench/turn_spots.json: 12 heads-up turn decisions (100 BB), the
# holdout for the Stage 5 tuning, which used the river spots only. Same syntax and
# conventions as gen_river_spots.py: scripts, four-card boards and assumed ranges,
# with the pot and stacks at the hero's decision computed independently of the engine.
import json, math, sys

def lround(x):
    return int(math.floor(x + 0.5))

POTS = {
    "btn_bb": dict(pre="UTG:F HJ:F CO:F BTN:R50 SB:F BB:C", oop="BB", ip="BTN",
                   name="BTN opens 2.5bb, BB calls (single-raised pot)"),
    "co_btn": dict(pre="UTG:F HJ:F CO:R50 BTN:C SB:F BB:F", oop="CO", ip="BTN",
                   name="CO opens 2.5bb, BTN calls (BTN in position)"),
    "sb_btn_3bet": dict(pre="UTG:F HJ:F CO:F BTN:R50 SB:R180 BB:F BTN:C", oop="SB", ip="BTN",
                        name="BTN opens 2.5bb, SB 3-bets to 9bb, BTN calls (3-bet pot)"),
}
AGGRESSOR = {"btn_bb": "ip", "co_btn": "oop", "sb_btn_3bet": "oop"}
FAMILIES = ["dry", "wet", "paired", "monotone"]
# Turn boards: the river bench's flops; dry and paired turns add a second suit
# (a flush draw), so every spot has draws.
BOARDS = {
    ("btn_bb", "dry"): "Kd 7c 2h 4d", ("co_btn", "dry"): "As 8d 3c 5d", ("sb_btn_3bet", "dry"): "Kc 8h 3d 2c",
    ("btn_bb", "wet"): "Jh Ts 8h 4c", ("co_btn", "wet"): "9d 8d 6c 2s", ("sb_btn_3bet", "wet"): "Th 9h 7c 6s",
    ("btn_bb", "paired"): "7c 7d 4s Jd", ("co_btn", "paired"): "Qs Qd 6c 3d",
    ("sb_btn_3bet", "paired"): "8s 8c 3d Kc",
    ("btn_bb", "monotone"): "Qh 8h 3h 5c", ("co_btn", "monotone"): "9s 6s 2s Kh",
    ("sb_btn_3bet", "monotone"): "Kd 9d 5d 2c",
}
# (line, hero role) per spot, balanced: every pot type and every hero role has two
# spots of each line, and every board family has one spot of each role.
# Lines: xx = flop checked through, bc = flop bet 33% and called.
# Roles: oop = hero OOP first to act; ip50 / ip75 = hero IP facing OOP's 50% / 75% turn bet.
PLAN = {
    ("btn_bb", "dry"): ("xx", "oop"), ("btn_bb", "wet"): ("bc", "ip50"),
    ("btn_bb", "paired"): ("xx", "ip75"), ("btn_bb", "monotone"): ("bc", "oop"),
    ("co_btn", "dry"): ("xx", "ip50"), ("co_btn", "wet"): ("bc", "ip75"),
    ("co_btn", "paired"): ("bc", "oop"), ("co_btn", "monotone"): ("xx", "ip50"),
    ("sb_btn_3bet", "dry"): ("xx", "ip75"), ("sb_btn_3bet", "wet"): ("xx", "oop"),
    ("sb_btn_3bet", "paired"): ("bc", "ip50"), ("sb_btn_3bet", "monotone"): ("bc", "ip75"),
}

R = {}
def r(pot, fam, oop, ip, note):
    R[(pot, fam)] = (oop, ip, note)

# ---------------- BTN vs BB ----------------
r("btn_bb", "dry",
  "99-22, 77:0.7, 22:0.7, ATs-A6s, KQs-K2s:0.85, Q5s+, J6s+, T6s+, 96s+, 85s+, 74s+, 63s+, 53s+, 43s, AJo-A4o, KQo-K8o:0.85, Q9o+, J9o+, T8o+, 98o, 87o",
  "AA:0.2, KK:0.25, QQ-JJ:0.3, TT-88:0.45, 77:0.2, 66-55:0.6, 44:0.5, 33:0.6, 22:0.2, AKs:0.15, AKo:0.15, KQs-K8s:0.25, KQo-K8o:0.25, K7s:0.2, K6s-K2s:0.4, AQs-A2s:0.45, AQo-A2o:0.45, QJs-Q2s:0.55, JTs-J4s:0.55, T9s-T6s:0.55, 98s-96s:0.55, 87s-85s:0.55, 76s-74s:0.3, 65s-64s:0.55, 54s-53s:0.55, 43s:0.55, QJo-Q9o:0.55, JTo-J9o:0.55, T9o-T8o:0.55, 98o:0.55",
  "Flop checked through; BB keeps nearly its whole defend range (some sets and Kx check-raised). BTN, which c-bets this flop with most hands, checked back weak pairs, high cards, the turned diamond and straight draws, and a few traps.")
r("btn_bb", "wet",
  "Q9s, Q9o:0.8, 97s:0.8, JTs:0.6, JTo:0.6, J8s:0.6, T8s:0.6, 88:0.6, 44:0.8, J9s:0.6, J9o:0.3, T9s:0.5, T9o:0.3, KJs:0.3, QJs:0.3, AJo:0.2, KQo:0.3, KQs:0.3, K9s:0.4, K9o:0.25, A9o:0.15, Q7s:0.25, T7s:0.25, 98s:0.4, 98o:0.25, 87s:0.35, 87o:0.15, 86s:0.2, 96s:0.35, 76s:0.45, 75s:0.2, KQs@h:0.7, KTs@h:0.6, QTs@h:0.6, K9s@h:0.7, A9s@h:0.7, A7s@h:0.6, A6s@h:0.6, K7s@h:0.5, K6s@h:0.4, Q7s@h:0.4, 97s@h, 96s@h:0.6, 76s@h:0.6, 75s@h:0.4, 65s@h:0.4, J9s@h:0.8, T9s@h:0.7, 98s@h:0.6, 87s@h:0.5",
  "AA-QQ, JJ:0.5, TT:0.5, 99:0.6, 88:0.5, 77-55:0.3, 44:0.4, AKs, AKo, AQs, AQo, AJs, AJo, ATs, ATo:0.8, A9s:0.5, A8s:0.5, KQs, KQo, KJs, KJo, KTs, K9s:0.6, QJs, QJo, QTs, Q9s, Q9o, JTs, J9s, J9o, J8s:0.8, T9s, T9o, T8s:0.8, T8o:0.6, 98s, 98o:0.7, 97s, 87s:0.6, 86s:0.4, 76s:0.4, 75s:0.3, A7s-A2s@h, K8s-K2s@h:0.6, Q8s-Q2s@h:0.5, J7s-J4s@h:0.8, T7s-T6s@h:0.8, 65s@h:0.5, 64s@h:0.4, 54s@h:0.4",
  "BB called a small c-bet on a wet flop and leads half pot on the 4c: a merged range of straights, two pair and sets, pairs with draws, and heart and straight draws. BTN c-bet most of its range: overpairs, Jx, Tx, straights, draws and air.")
r("btn_bb", "paired",
  "A7s, 87s, 97s, 76s, 75s, 74s, 87o, 44, 77, KJs:0.5, QJs:0.5, JTs:0.4, J9s:0.4, AJo:0.5, KJo:0.5, QJo:0.3, JTo:0.25, J9o:0.25, 65s, 53s:0.6, 63s:0.5, 86s:0.5, 85s:0.4, T9s:0.5, T8s:0.4, 98s:0.5, 96s:0.3, T9o:0.25, T8o:0.15, 98o:0.25, K9s-K2s:0.12, Q9s-Q5s:0.12, K9o-K8o:0.1, Q9o:0.12, A9s@d:0.6, A8s@d:0.6, A6s@d:0.6, A5s@d:0.5, A3s@d:0.5, A2s@d:0.5, K9s@d:0.5, K8s@d:0.5, K6s@d:0.5, K5s@d:0.4, K3s@d:0.4, Q9s@d:0.5, Q8s@d:0.5, Q6s@d:0.4, Q5s@d:0.4, T9s@d:0.6, T8s@d:0.5, 98s@d:0.5, 96s@d:0.4, 65s@d, 63s@d:0.5",
  "AA:0.2, KK:0.2, QQ:0.25, JJ:0.25, TT-88:0.5, 66-55:0.6, 44:0.3, 33-22:0.6, A7s:0.25, 87s:0.3, 97s:0.3, 76s:0.3, 75s:0.3, 74s:0.3, AKs:0.3, AKo:0.3, AQs-A8s:0.45, AQo-A8o:0.45, A6s-A2s:0.45, A7o-A2o:0.4, KQs-K8s:0.45, KQo-K8o:0.45, K7s:0.3, K6s-K2s:0.4, QJs-Q8s:0.5, Q6s-Q2s:0.4, QJo-Q9o:0.5, JTs-J8s:0.55, J6s-J4s:0.55, JTo-J9o:0.55, T9s-T8s:0.55, T6s:0.5, T9o-T8o:0.5, 98s:0.5, 96s:0.5, 98o:0.5, 86s-85s:0.5, 65s-64s:0.5, 54s-53s:0.5, 43s:0.4",
  "Flop checked through; BB probes the turn jack 75%: polarised between trips, full houses and strong Jx and diamond and straight draws. BTN checked back pairs, high cards and some trips.")
r("btn_bb", "monotone",
  "QJs:0.8, QTs:0.8, Q9s:0.8, Q7s-Q5s:0.7, QJo:0.8, QTo:0.8, Q9o:0.8, KQs:0.6, KQo:0.7, 88:0.5, 33:0.5, 99:0.6, 77-66:0.6, 55:0.7, 44:0.5, 22:0.5, ATs-A6s:0.3, AJo-A9o:0.3, A8o:0.6, 98s:0.6, 87s:0.6, 86s:0.6, 85s:0.5, T8s:0.5, J8s:0.5, 98o:0.5, 87o:0.5, T8o:0.4, 76s:0.6, 75s:0.4, 74s:0.5, 64s:0.5, 54s:0.4, 65s:0.4, ATs-A6s@h:0.8, KJs-K2s@h:0.7, JTs-J6s@h:0.7, T9s-T6s@h:0.7, 97s@h:0.6, 96s@h:0.6, 76s@h:0.6, 75s@h:0.6, 74s@h:0.6, 65s@h:0.6, 64s@h:0.6, 54s@h:0.6, 63s@h:0.5, 43s@h:0.5, AJo-A4o@h:0.6, KJo-K8o@h:0.5, QJs@h:0.6, QTs@h:0.6",
  "AA-KK, QQ:0.3, JJ-99:0.6, 88:0.3, 33:0.3, 77-66:0.4, 55:0.5, 44:0.3, 22:0.3, AQs, AQo, KQs, KQo, QJs, QJo, QTs, QTo:0.8, Q9s, Q9o, Q7s-Q4s:0.6, AKo:0.6, AJs:0.4, AJo:0.4, ATo:0.3, KJs:0.4, JTs:0.4, T9s:0.4, 98s:0.5, 87s:0.5, 76s:0.4, 65s:0.4, 54s:0.4, A2s+@h, KJs-K2s@h, J4s+@h:0.8, T6s+@h:0.8, 97s@h, 96s@h, 76s@h, 75s@h, 74s@h, 65s@h, 64s@h, 54s@h, AKo@h, AJo-A2o@h:0.8, KJo-K8o@h:0.6",
  "BB check-called a small c-bet on a monotone flop: Qx, 8x, pairs, some flushes (others check-raised), one-heart draws, the turned set of fives and straight draws. BTN c-bet Qx, overpairs, flushes, heart draws and air.")

# ---------------- CO vs BTN ----------------
r("co_btn", "dry",
  "A9s-A6s:0.5, A4s:0.6, A2s:0.6, A5s:0.7, A9o:0.4, AJo:0.3, ATo:0.3, 55:0.7, 88:0.3, 33:0.3, 65s:0.8, 54s:0.8, 75s:0.6, 76s:0.6, 98s:0.4, 87s:0.5, 86s:0.4, 77-66:0.3, KQo:0.3, KJo:0.25, KTo:0.2, QJo:0.2, QTo:0.15, JTo:0.15, KQs:0.3, KJs:0.3, KTs:0.25, K9s-K5s:0.2, QJs:0.25, QTs:0.2, JTs:0.25, T9s:0.2, 97s:0.4, KQs@d:0.7, KJs@d:0.7, KTs@d:0.7, K9s@d:0.6, K7s@d:0.6, QJs@d:0.6, QTs@d:0.6, Q9s@d:0.5, JTs@d:0.6, J9s@d:0.5, T9s@d:0.5, 97s@d:0.5, 76s@d:0.7",
  "TT-99:0.8, 77-66:0.8, 55:0.3, 44:0.8, 88:0.3, 33:0.2, 22:0.4, AJs:0.4, ATs:0.5, A9s:0.5, A8s:0.3, AJo:0.4, KTs, QJs, QTs, KQs:0.3, KQo, J9s, JTs, T9s, 98s:0.7, 87s:0.8, 76s:0.4, 65s:0.4",
  "CO skipped the c-bet on an ace-high flop and bets half pot on the 5d after BTN checked back: weak aces, turned fives and wheel and diamond draws. BTN checked back pairs below the ace, high cards and a few weak aces.")
r("co_btn", "wet",
  "AA:0.5, KK:0.5, QQ:0.5, JJ:0.4, TT:0.4, 99, 88:0.8, 66, 22:0.6, 98s, 75s, 97s:0.7, 87s:0.6, 86s:0.6, A9s:0.3, JTs:0.7, T9s:0.5, 76s:0.5, 65s:0.6, 54s:0.6, QJs:0.4, KQs:0.4, KJs:0.3, KTs:0.3, KQo:0.3, AKo:0.3, AQo:0.3, AJo:0.2, A5s:0.4, A4s:0.4, AKs@d, AQs@d, AJs@d, ATs@d, A5s@d:0.8, A4s@d:0.8, A3s@d:0.8, A2s@d:0.6, KQs@d:0.8, KJs@d:0.8, KTs@d:0.8, QJs@d:0.7, QTs@d:0.7, JTs@d:0.8, 76s@d",
  "TT:0.6, 99:0.4, 88:0.5, 77:0.8, 66:0.5, 55:0.6, 44:0.4, 33:0.3, 22:0.5, AJs:0.4, ATs:0.5, A9s:0.9, A8s:0.9, AJo:0.3, KTs:0.4, QJs:0.4, QTs:0.5, JTs, J9s, T9s, 98s:0.8, 87s:0.9, 76s:0.6, 65s:0.4, KQo:0.2, KTs@d:0.8, QJs@d, QTs@d, JTs@d, AJs@d, ATs@d, KQo@d:0.6",
  "CO c-bet small, BTN called, and CO barrels 75% on the 2s: overpairs, sets, two pair and straights against diamond and straight draws. BTN called with pairs, straight draws and diamond draws.")
r("co_btn", "paired",
  "22+:0.85, A2s+:0.85, K5s+:0.85, Q8s+, J8s+:0.8, T8s+:0.8, 97s+:0.8, 86s+:0.8, 75s+:0.8, 65s:0.8, 54s:0.8, A8o+:0.85, KTo+:0.85, QTo+, JTo:0.8",
  "JJ:0.2, TT-77:0.9, 66, 55-44:0.6, 33:0.8, 22:0.3, AJs-A8s:0.7, AJo:0.6, KQo:0.9, QJs, QTs, KTs:0.5, JTs:0.5, J9s:0.4, T9s:0.4, 98s:0.3, 87s:0.3, 76s:0.4, 65s:0.6, AJs@d, ATs@d, A9s@d, A8s@d, KTs@d, JTs@d, J9s@d, T9s@d, 98s@d, 87s@d, 76s@d, 65s@d",
  "CO c-bet a paired flop small with nearly its whole range and BTN called with queens, pairs, A-high and backdoor draws; the 3d adds a diamond draw. CO is first to act on the turn.")
r("co_btn", "monotone",
  "KQs:0.6, KQo:0.6, KJs:0.5, KJo:0.5, KTs:0.5, KTo:0.5, K9s:0.8, K8s-K5s:0.5, 99:0.6, 66:0.6, 22:0.5, AA:0.4, AKs:0.6, AKo:0.6, A9s:0.3, T9s:0.4, 98s:0.4, 87s:0.7, 75s:0.5, T8s:0.5, AQo:0.4, AJo:0.4, ATo:0.4, AQs:0.4, AJs:0.4, ATs:0.4, QJo:0.4, QTo:0.4, JTo:0.4, QJs:0.4, QTs:0.4, JTs:0.4, J8s:0.4, 54s:0.4, AQs@s, AJs@s, ATs@s, A8s-A2s@s:0.8, AQo@s:0.6, AJo@s:0.6, ATo@s:0.5, A9o@s:0.5, A8o@s:0.5, KQs@s:0.5, KJs@s:0.5, QJs@s:0.6, QTs@s:0.6, JTs@s:0.6, J8s@s:0.6, T8s@s:0.6, 87s@s:0.7, 75s@s:0.7, 54s@s:0.6, Q8s@s:0.5",
  "TT-77:0.6, 99:0.3, 66:0.3, 55-44:0.6, 33-22:0.4, 88:0.6, AJs:0.6, ATs:0.6, A9s:0.4, A8s:0.6, AJo:0.6, KTs:0.7, KQo:0.7, QJs:0.6, QTs:0.6, JTs:0.6, J9s:0.5, T9s:0.5, 98s:0.5, 87s:0.6, 76s:0.4, 65s:0.4, AJs@s:0.5, ATs@s:0.5, A8s@s:0.5, KTs@s:0.6, QJs@s:0.5, QTs@s:0.5, JTs@s:0.5, 87s@s:0.4",
  "Monotone flop checked through; CO bets half pot on the Kh: Kx, sets, some slowplayed flushes and spade and straight draws. BTN checked back pairs, high cards, weak flushes and draws.")

# ---------------- SB 3-bet vs BTN ----------------
r("sb_btn_3bet", "dry",
  "KK:0.3, 88:0.5, AA:0.4, AKs:0.5, AKo:0.5, KQs:0.5, KJs:0.4, KJo:0.3, KTs:0.3, QQ:0.2, A5s:0.7, A4s:0.7, A3s:0.4, AQo:0.25, AJo:0.25, ATo:0.15, AQs:0.15, AJs:0.15, ATs:0.15, A9s:0.15, QJs:0.25, QTs:0.25, JTs:0.25, T9s:0.25, AQs@c, AJs@c, ATs@c, A9s@c, QJs@c, QTs@c, JTs@c, T9s@c, AQo@c:0.3, A5s@c, A4s@c",
  "KQs:0.5, KQo:0.25, KJs:0.5, KTs:0.4, AKs:0.06, AKo:0.06, KK:0.05, QQ:0.15, JJ-99:0.8, 77-55:0.6, 88:0.3, AQs:0.8, AQo:0.8, AJs:0.8, AJo:0.4, ATs:0.8, A9s:0.4, QJs:0.8, QTs:0.4, JTs:0.8, T9s:0.8, 98s:0.4",
  "The 3-bettor checked a dry flop, BTN checked back, and SB bets 75% on the 2c: Kx, sets and overpairs against club and wheel draws. BTN checked back pairs, A-high and some Kx.")
r("sb_btn_3bet", "wet",
  "AA:0.7, KK:0.7, QQ:0.7, JJ:0.7, TT:0.4, 99:0.4, 88:0.6, AKs:0.6, AKo:0.6, AQs:0.6, AJs:0.6, ATs:0.6, A9s:0.6, A8s:0.5, AQo:0.6, AJo:0.6, ATo:0.6, A5s-A2s:0.5, KQs:0.6, KJs:0.6, KTs:0.6, KQo:0.6, KJo:0.6, QJs:0.5, QTs:0.6, JTs:0.5, T9s:0.4, AKs@h:0.8, AQs@h:0.8, AJs@h:0.8, A5s@h:0.8, KQs@h:0.8, KJs@h:0.8, QJs@h:0.8",
  "JJ:0.5, QQ:0.1, 88:0.5, 77:0.3, 66:0.5, 55:0.6, TT:0.15, 99:0.15, AQs:0.6, AQo:0.6, AJs:0.6, AJo:0.3, A9s:0.5, ATs:0.4, KQs:0.6, KQo:0.25, KJs:0.6, KTs:0.4, QJs:0.4, QTs:0.4, JTs:0.4, T9s:0.2, 98s:0.3, AKs:0.06, AKo:0.06, AQs@h:0.6, AJs@h:0.6, KQs@h:0.5, KJs@h:0.5, QJs@h:0.5",
  "The 3-bettor checked a wet flop and BTN checked back; the 6s turn makes 8x a straight. SB is first to act with overpairs, A-high, Tx, heart draws and a few sets and straights. BTN checked back medium pairs, A-high and draws.")
r("sb_btn_3bet", "paired",
  "KK, AA:0.7, AKs, AKo, KQs, KJs:0.8, KTs:0.6, KQo, KJo:0.6, QQ:0.6, JJ:0.5, TT:0.4, 99:0.4, A8s, 88, A5s:0.5, A4s:0.5, A3s:0.4, A2s:0.4, AQs:0.4, AQo:0.4, AJs:0.3, AJo:0.3, ATo:0.3, QJs:0.4, JTs:0.4, T9s:0.4, AQs@c, AJs@c, ATs@c, QJs@c, QTs@c, JTs@c, T9s@c, A5s@c, A4s@c, A3s@c:0.6, A2s@c",
  "JJ-99:0.9, 77-66:0.8, 55:0.4, QQ:0.2, KK:0.1, AKs:0.2, AKo:0.15, AQs, AQo, AJs:0.8, AJo:0.4, ATs:0.6, A9s:0.4, KQs, KQo:0.5, KJs, KTs:0.7, QJs:0.5, QTs:0.4, JTs:0.5, T9s:0.5, 98s:0.5, AQs@c, AJs@c, ATs@c, A9s@c, QJs@c, QTs@c, JTs@c, T9s@c, 98s@c",
  "SB c-bet the paired flop, BTN called, and SB barrels half pot on the Kc: Kx, overpairs, trips and club draws. BTN called with pairs, A-high, Kx and backdoor draws.")
r("sb_btn_3bet", "monotone",
  "AKs:0.6, AKo:0.5, KQs:0.5, KK, 99, AA:0.4, T9s:0.4, A5s:0.4, A4s:0.4, AQo:0.3, AJo:0.3, QJs:0.3, JTs:0.3, QQ:0.3, JJ:0.3, TT:0.3, AQs@d, AJs@d, ATs@d, A8s@d, A4s-A2s@d, QJs@d, QTs@d, JTs@d, KQs@d, KJs@d, KTs@d, AQo@d:0.6, AJo@d:0.6, ATo@d:0.6, QQ@d:0.6, JJ@d:0.6, TT@d:0.6",
  "KQs:0.8, KQo:0.4, KJs:0.8, KTs:0.6, AKs:0.15, AKo:0.15, QQ-TT:0.5, 99:0.3, 88-66:0.5, 55:0.3, A9s:0.4, T9s:0.6, 98s:0.4, QQ@d:0.6, JJ@d:0.6, TT@d:0.6, AQs@d, AJs@d, ATs@d, A9s@d, QJs@d, QTs@d, JTs@d, T9s@d, 98s@d, AQo@d:0.8, AJo@d:0.5, KQo@d:0.6",
  "SB c-bet the monotone flop and barrels 75% on the 2c: flushes, sets and Kx against one-diamond draws. BTN called with Kx, pairs, flushes and diamond draws.")

def build():
    spots = []
    for pot, P in POTS.items():
        O, I = P["oop"], P["ip"]
        agg = O if AGGRESSOR[pot] == "oop" else I
        for fam in FAMILIES:
            line, role = PLAN[(pot, fam)]
            if line == "xx":
                flop = f"{O}:X {I}:X"
            else:
                flop = f"{agg}:B33% {I}:C" if agg == O else f"{O}:X {I}:B33% {O}:C"
            turn = "" if role == "oop" else f"{O}:B{role[2:]}%"
            oop_range, ip_range, note = R[(pot, fam)]
            line_text = {"xx": "flop checked through", "bc": "flop bet-call"}[line]
            hero_text = "hero OOP first to act" if role == "oop" else f"hero IP facing a {role[2:]}% turn bet"
            spot = {
                "id": f"{pot}-{fam}-{line}",
                "description": f"{P['name']}; {fam} board, {line_text}; {hero_text}. {note}",
                "pot_type": pot, "family": fam, "line": line,
                "board": BOARDS[(pot, fam)],
                "pre": P["pre"], "flop": flop, "turn": turn,
                "hero": "oop" if role == "oop" else "ip",
                "oop_range": oop_range, "ip_range": ip_range,
            }
            spot.update(expected(spot, O, I))
            spots.append(spot)
    return spots

ORDER_PRE = ["UTG", "HJ", "CO", "BTN", "SB", "BB"]
def expected(spot, O, I):
    invested = {p: 0 for p in ORDER_PRE}
    invested["SB"] = 10; invested["BB"] = 20
    stack = {p: 2000 - invested[p] for p in ORDER_PRE}
    for street in ["pre", "flop", "turn"]:
        bets = {p: 0 for p in ORDER_PRE}
        if street == "pre":
            bets["SB"] = 10; bets["BB"] = 20
        current = max(bets.values())
        for tok in spot[street].split():
            pos, act = tok.split(":")
            pot = sum(invested.values())
            if act == "F" or act == "X":
                continue
            if act == "C":
                add = current - bets[pos]
            else:
                amt = act[1:]
                if amt.endswith("%"):
                    facing = current - bets[pos]
                    to = current + lround(float(amt[:-1]) / 100.0 * (pot + facing))
                else:
                    to = int(amt)
                add = to - bets[pos]
                current = to
            bets[pos] += add; invested[pos] += add; stack[pos] -= add
    return {"pot": sum(invested.values()), "stacks": [stack[O], stack[I]]}

spots = build()
fmt = ("See tools/bench/README.md. Turn decisions: the board has four cards and there is no river "
       "script. Ranges are assumed, not solved: each is the player's range at the hero's turn decision.")
out = sys.argv[1]
with open(out, "w") as f:
    f.write("{\n")
    f.write('  "format": ' + json.dumps(fmt) + ",\n")
    f.write('  "street": "turn",\n')
    f.write('  "stack": 2000,\n  "small_blind": 10,\n  "big_blind": 20,\n  "spots": [\n')
    for i, s in enumerate(spots):
        f.write("    {\n")
        items = list(s.items())
        for j, (k, v) in enumerate(items):
            f.write(f'      "{k}": {json.dumps(v)}' + ("," if j + 1 < len(items) else "") + "\n")
        f.write("    }" + ("," if i + 1 < len(spots) else "") + "\n")
    f.write("  ]\n}\n")
print(len(spots), "spots")
from collections import Counter
print(Counter((s["pot_type"], s["line"], s["hero"] + s["turn"][-4:]) for s in spots))
