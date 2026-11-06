#!/usr/bin/env python3
# Builds tools/bench/river_spots.json: scripts, boards and assumed ranges, with the
# pot and stacks at the hero's decision computed independently of the engine.
import json, math, sys

def lround(x):
    return int(math.floor(x + 0.5))

POTS = {
    "btn_bb": dict(pre="UTG:F HJ:F CO:F BTN:R50 SB:F BB:C", oop="BB", ip="BTN", turn_bet="75%",
                   name="BTN opens 2.5bb, BB calls (single-raised pot)"),
    "co_btn": dict(pre="UTG:F HJ:F CO:R50 BTN:C SB:F BB:F", oop="CO", ip="BTN", turn_bet="75%",
                   name="CO opens 2.5bb, BTN calls (BTN in position)"),
    "sb_btn_3bet": dict(pre="UTG:F HJ:F CO:F BTN:R50 SB:R180 BB:F BTN:C", oop="SB", ip="BTN", turn_bet="50%",
                        name="BTN opens 2.5bb, SB 3-bets to 9bb, BTN calls (3-bet pot)"),
}
AGGRESSOR = {"btn_bb": "ip", "co_btn": "oop", "sb_btn_3bet": "oop"}
FAMILIES = ["dry", "wet", "paired", "monotone"]
LINES = ["xx", "bx", "bb"]
BOARDS = {
    ("btn_bb", "dry"): "Kd 7c 2h 4s 9d", ("co_btn", "dry"): "As 8d 3c 5h Qs", ("sb_btn_3bet", "dry"): "Kc 8h 3d 2s 6c",
    ("btn_bb", "wet"): "Jh Ts 8h 4c 2s", ("co_btn", "wet"): "9d 8d 6c 2s Ad", ("sb_btn_3bet", "wet"): "Th 9h 7c 6s 2c",
    ("btn_bb", "paired"): "7c 7d 4s Jh 2c", ("co_btn", "paired"): "Qs Qd 6c 3h 9s",
    ("sb_btn_3bet", "paired"): "8s 8c 3d Kc 2h",
    ("btn_bb", "monotone"): "Qh 8h 3h 5c Kd", ("co_btn", "monotone"): "9s 6s 2s Kh Jd",
    ("sb_btn_3bet", "monotone"): "Kd 9d 5d 2c 7s",
}
ROLES = ["oop", "ip33", "ip75"]

R = {}
def r(pot, fam, line, oop, ip, note):
    R[(pot, fam, line)] = (oop, ip, note)

# ---------------- BTN vs BB ----------------
r("btn_bb", "dry", "xx",
  "99-22, 99:0.8, 77:0.6, 44:0.5, 22:0.6, ATs-A8s, A7s:0.7, A6s, KJs-K2s:0.5, QJs-Q5s, JTs-J6s, T9s-T6s, 98s-96s, 97s:0.8, 87s:0.7, 86s, 85s, 76s:0.7, 75s:0.7, 74s:0.6, 65s:0.8, 64s:0.7, 63s, 54s:0.7, 53s, 43s:0.7, AJo-A4o, KQo-K8o:0.5, QJo-Q9o, JTo-J9o, T9o-T8o, 98o, 87o:0.7",
  "KK:0.5, JJ-TT:0.3, 99:0.5, 88:0.6, 77:0.25, 66-55:0.7, 44:0.25, 33:0.7, 22:0.25, K6s-K3s:0.3, K8o:0.3, A9s:0.6, A8s-A5s:0.4, A7s:0.5, A4s:0.5, A3s:0.4, A2s:0.5, AJo-A8o:0.3, A9o:0.6, QJs:0.5, QTs:0.5, Q9s:0.6, Q8s-Q3s:0.4, JTs:0.5, J9s:0.6, J8s-J4s:0.4, T9s:0.6, T8s:0.4, T6s:0.4, 98s:0.6, 97s:0.5, 96s:0.6, 87s:0.7, 86s:0.5, 85s:0.5, 76s:0.7, 75s:0.7, 74s:0.4, 65s:0.5, 64s:0.5, 54s:0.5, 53s:0.5, 43s:0.5, QJo:0.4, QTo:0.4, Q9o:0.6, JTo:0.4, J9o:0.6, T9o:0.6, T8o:0.4, 98o:0.6",
  "Both checked twice: BB keeps most of its defend range (Kx and 7x probed the turn some of the time); BTN, which bets this flop with most of its range, holds weak pairs, missed high cards and a few traps.")
r("btn_bb", "dry", "bx",
  "KJs-K2s:0.6, KQo:0.4, KJo:0.5, KTo-K8o:0.6, 99:0.5, 77:0.5, 44:0.5, 22:0.5, A9s:0.5, A7s:0.5, 97s:0.8, 98s:0.6, 98o:0.6, 87s:0.5, 87o:0.5, 76s:0.5, 75s:0.5, 74s:0.3, J9s:0.2, T9s:0.2, Q9s:0.2, 65s:0.4, 64s:0.4, 63s:0.4, 53s:0.4, 86s:0.3, 85s:0.3, 43s:0.3, QJs:0.25, QTs:0.25, JTs:0.25, T8s:0.3, A8s:0.3, A6s:0.3",
  "KK:0.2, AKs:0.2, AKo:0.2, KQs:0.3, KQo:0.3, KJs:0.6, KJo:0.6, KTs-K3s:0.8, KTo-K8o:0.8, QQ-88:0.7, 66-33:0.6, 77:0.15, 44:0.2, 22:0.15, AQs-ATs:0.6, AQo-ATo:0.6, A9s:0.6, A9o:0.6, A8s:0.5, A8o:0.5, A7s:0.6, A6s-A3s:0.4, QJs:0.4, QTs:0.4, Q9s:0.5, JTs:0.4, J9s:0.5, T9s:0.5, 98s:0.5, 97s:0.5, 87s:0.6, 76s:0.6, 75s:0.6, 65s:0.3, 86s:0.3, 85s:0.3, QJo:0.4, QTo:0.4, Q9o:0.5, JTo:0.4, J9o:0.5, T9o:0.5, 98o:0.5",
  "BB called a small c-bet and the turn checked through; BB's third-of-pot lead is thin value (weak Kx, 7x, 9x two pair, small sets) plus missed straight draws. BTN's turn check-back is medium Kx, pairs and give-ups.")
r("btn_bb", "dry", "bb",
  "K9s, K9o, 99, 97s, 77:0.7, 44:0.7, 22:0.7, KJs:0.5, KTs:0.4, KJo:0.4, KTo:0.4, 65s, 63s:0.8, 53s:0.8, 86s:0.7, 85s:0.6, 43s:0.3",
  "KK:0.5, 77:0.6, 44:0.6, 22:0.6, AKs, AKo, KQs, KQo, KJs, KJo, KTs:0.8, KTo:0.7, K9s, K9o:0.8, K8s:0.4, K8o:0.3, K7s:0.6, QJs:0.6, QTs:0.6, JTs:0.6, Q9s:0.4, J9s:0.4, T9s:0.4, Q8s:0.3, J8s:0.3, T8s:0.4, 65s:0.8, 53s:0.6, 86s:0.6, 85s:0.4, A5s:0.6, A3s:0.6, 43s:0.3",
  "BB check-called two barrels then leads big: polarised between two pair or sets and missed straight draws. BTN barrelled with top pair and better plus draws, which missed.")
r("btn_bb", "wet", "xx",
  "J9s:0.6, J7s:0.6, J6s:0.4, J9o:0.6, T9s:0.7, T7s:0.6, T6s:0.5, T9o:0.6, KTs:0.6, QTs:0.6, ATs:0.5, A8s:0.4, 98s:0.6, 98o:0.5, 87s:0.6, 87o:0.5, 86s:0.4, 85s:0.4, 99:0.5, 77-55:0.3, JTs:0.4, JTo:0.4, 88:0.3, 44:0.3, 22:0.15, KJs:0.5, QJs:0.5, KJo:0.4, QJo:0.4, AJo:0.4, 97s:0.3, Q9s:0.3, Q9o:0.3, 76s:0.4, 96s:0.4, A7s@h:0.5, A6s@h:0.5, K6s@h:0.3, K5s@h:0.3, K3s@h:0.3, Q6s@h:0.3, Q5s@h:0.3, 65s@h:0.3",
  "AJs, AJo:0.7, KJs:0.7, KJo:0.6, QJs:0.6, QJo:0.5, J9s:0.6, J9o:0.5, J7s-J4s:0.4, JTs:0.4, JTo:0.4, 88:0.3, TT:0.3, JJ:0.2, Q9s:0.3, Q9o:0.3, 97s:0.3, KQs:0.6, KQo:0.5, A9s:0.5, A9o:0.3, K9s:0.5, K9o:0.4, 96s:0.6, 76s:0.5, T9s:0.5, KTs:0.4, QTs:0.4, ATo:0.3, T9o:0.4, 98s:0.4, 98o:0.4, 87s:0.4, AKs@h:0.8, AQs@h, A5s@h:0.6, A3s@h:0.6, K6s@h:0.5, K5s@h:0.5, Q6s@h:0.4, Q5s@h:0.4, 75s@h:0.4, 65s@h:0.4",
  "Flop checked through, BTN stabbed the turn and BB called; every draw bricked. BB's small lead is middling pairs with a few missed heart draws; BTN holds pairs, straights it slowplayed and many missed draws.")
r("btn_bb", "wet", "bx",
  "Q9s, Q9o, 97s, JTs, JTo:0.8, J8s:0.8, T8s:0.8, 88:0.8, 44:0.5, 22:0.4, KJs:0.4, QJs:0.4, AJo:0.3, KJo:0.3, 76s:0.6, 96s:0.6, K9s:0.5, K9o:0.3, A9o:0.3, Q7s:0.5, A7s@h:0.7, A6s@h:0.7, K6s@h:0.5, K5s@h:0.5, K3s@h:0.5, Q6s@h:0.4, Q5s@h:0.4, 65s@h:0.5",
  "AJs:0.4, AJo:0.4, KJs:0.5, KJo:0.5, QJs:0.6, QJo:0.6, J9s:0.7, J9o:0.7, J7s-J4s:0.6, KTs:0.8, KTo:0.7, QTs:0.8, QTo:0.7, ATs:0.7, ATo:0.7, T9s:0.6, T9o:0.6, T7s:0.7, T6s:0.7, 98s:0.7, 98o:0.7, 87s:0.7, 86s:0.6, 85s:0.6, A8s:0.6, 99:0.8, 77-55:0.6, 33:0.6, 88:0.2, TT:0.2, JJ:0.2, AKs:0.4, AKo:0.4, AQs:0.4, AQo:0.4, KQs:0.4, KQo:0.4, 96s:0.5, 76s:0.5, Q9s:0.2, AKs@h:0.6, AQs@h:0.6, A5s@h:0.6, A3s@h:0.6",
  "BB called a small c-bet on a wet flop, the turn checked through and BB leads big on a brick river: straights, two pair and sets against missed heart and straight draws. BTN's turn check-back is pairs and free-card draws.")
r("btn_bb", "wet", "bb",
  "Q9s, Q9o:0.8, 97s:0.7, JTs:0.8, JTo:0.8, J8s:0.8, T8s:0.8, 88:0.8, 44:0.7, 22:0.1, KJs:0.9, KJo:0.8, QJs:0.9, QJo:0.8, AJo:0.8, J9s:0.9, J9o:0.8, J7s:0.6, J6s:0.5, T9s:0.6, KTs:0.3, QTs:0.3, ATs:0.3, Q7s:0.5, 96s:0.6, 76s:0.6, K9s:0.6, K9o:0.4, 99:0.3, 98s:0.5, 87s:0.4, A8s@h, A7s@h, A6s@h, K6s@h:0.8, K5s@h:0.8, K3s@h:0.6, K2s@h:0.6, Q6s@h:0.6, Q5s@h:0.6",
  "AJs, AJo, KJs, KJo:0.8, QJs:0.6, QJo:0.5, J9s:0.5, JJ:0.6, TT:0.6, 88:0.6, 44:0.4, JTs:0.8, JTo:0.8, J8s:0.8, T8s:0.6, Q9s, Q9o, 97s, KQs, KQo:0.8, AKs:0.5, AQs:0.3, AKo:0.3, A9s:0.6, A9o:0.4, K9s:0.8, K9o:0.6, Q7s:0.5, 96s:0.7, 76s:0.6, AKs@h, AQs@h, A5s@h, A4s@h, A3s@h, A2s@h, K7s@h, K6s@h, K5s@h, K4s@h, Q6s@h, Q5s@h, 65s@h:0.6, 75s@h:0.5, 54s@h:0.5",
  "BB called two barrels on a wet board that bricked: Jx, two pair, straights and missed flush and straight draws. BTN barrelled value and draws, many of which missed.")
r("btn_bb", "paired", "xx",
  "A7s, 87s, 97s:0.8, 76s, 75s, 74s, 87o, 44, 22:0.8, 99:0.3, KJs:0.5, QJs:0.5, J9s:0.4, AJo:0.4, KJo:0.4, QJo:0.3, 65s:0.8, 53s:0.6, 63s:0.7, 86s:0.6, 85s:0.4, 96s:0.4, K5s:0.3, K3s:0.3, Q6s:0.3, Q5s:0.3",
  "A7s:0.2, 87s:0.3, 97s:0.3, 76s:0.3, 75s:0.3, 74s:0.3, 44:0.3, JJ:0.2, QQ:0.2, TT-88:0.5, 66-55:0.6, 33:0.6, 22:0.6, AKs:0.3, AKo:0.3, AQs:0.4, AQo:0.4, AJs:0.4, AJo:0.4, ATs-A8s:0.5, ATo-A8o:0.5, A5s:0.4, A3s:0.4, KQs:0.4, KQo:0.4, KJs:0.3, KTs:0.4, K9s:0.4, QJs:0.5, QTs:0.5, Q9s:0.5, JTs:0.6, J9s:0.6, J8s:0.6, J6s-J4s:0.6, J9o:0.6, QJo:0.5, T9s:0.5, 98s:0.5, 65s:0.5, 64s:0.5, 54s:0.5, 53s:0.5, 43s:0.4, K8o:0.4, T8o:0.4, 98o:0.5",
  "Checked flop and turn; BB leads big: trips, full houses and turned Jx against missed straight draws. BTN, which c-bets this flop with most hands, checked back pairs, high cards and a few traps.")
r("btn_bb", "paired", "bx",
  "99:0.9, 88:0.9, 66-55:0.9, 33:0.8, 22, 44:0.5, A7s:0.6, 87s:0.6, 97s:0.6, 76s:0.6, 75s:0.6, 74s:0.4, 87o:0.6, 54s, 64s, 43s, A4o, J9s:0.3, KJs:0.3, QJs:0.3, JTs:0.3, J9o:0.3, ATs-A8s:0.6, A6s:0.6, AJo-A8o:0.5, A5o:0.5, 65s, 63s, 53s, 86s:0.8, 85s:0.8, 96s:0.4",
  "A7s:0.3, 87s:0.3, 76s:0.3, 75s:0.3, 97s:0.3, 74s:0.3, 44:0.3, JJ:0.2, QQ:0.4, KK:0.3, AA:0.2, TT-88:0.7, 66-55:0.6, 33:0.6, 22:0.5, AJs:0.3, AJo:0.3, KJs:0.4, KJo:0.4, QJs:0.6, QJo:0.6, JTs:0.6, JTo:0.6, J9s:0.6, J9o:0.6, J8s:0.6, J6s-J5s:0.6, AKs:0.5, AKo:0.5, AQs:0.5, AQo:0.5, ATs-A8s:0.5, ATo-A8o:0.5, A6s-A2s:0.4, A6o-A2o:0.3, KQs:0.5, KQo:0.5, KTs-K8s:0.4, KTo-K8o:0.4, QTs:0.4, Q9s:0.4, T9s:0.4, 98s:0.4, 65s:0.4, 54s:0.4, 64s:0.4, 53s:0.4",
  "BB called a small c-bet with pairs, 7x, 4x, A-high and straight draws; the turn checked through. BTN's check-back range is pairs, weak Jx and A-high.")
r("btn_bb", "paired", "bb",
  "KJs:0.6, QJs:0.7, JTs:0.7, J9s:0.7, J8s:0.7, AJo:0.5, KJo:0.6, QJo:0.7, J9o:0.7, JTo:0.7, 99:0.5, 88:0.5, A7s:0.3, 87s:0.4, 97s:0.4, 76s:0.4, 75s:0.4, 87o:0.4, 65s:0.4, 63s:0.3, 53s:0.3, 86s:0.3, 22:0.1",
  "AA:0.8, KK:0.8, QQ:0.7, JJ:0.6, AJs, AJo, KJs, KJo:0.8, QJs:0.6, QJo:0.5, JTs:0.4, J9s:0.4, A7s:0.6, 87s:0.6, 76s:0.6, 75s:0.6, 97s:0.6, 74s:0.6, 44:0.6, 65s:0.8, 64s:0.4, 53s:0.6, 98s:0.6, T9s:0.6, 86s:0.5, 85s:0.4, AKs:0.4, AKo:0.3, AQs:0.4, KQs:0.4, TT:0.3, 99:0.3",
  "BB called two barrels and blocks small with medium Jx and pocket pairs, slowplaying a little trips. BTN barrelled overpairs, Jx, trips and straight draws.")
r("btn_bb", "monotone", "xx",
  "QJs:0.9, QTs:0.9, Q9s:0.9, Q7s-Q6s:0.8, Q5s:0.8, QJo:0.9, QTo:0.9, Q9o:0.9, KQo:0.9, KJs-K2s:0.15, KJo-K8o:0.15, 99:0.4, 77-66:0.4, 44:0.3, 22:0.3, 88:0.4, 33:0.4, 55:0.3, A8s:0.5, 98s:0.5, 87s:0.5, 86s:0.5, 85s:0.6, J8s:0.5, T8s:0.5, 98o:0.4, 87o:0.4, T8o:0.4, 65s:0.5, 54s:0.5, 75s:0.5, ATs-A6s@h:0.4, KJs-K2s@h:0.4, JTs-J6s@h:0.4, T9s-T6s@h:0.4, 97s@h:0.4, 96s@h:0.4, 76s@h:0.4, 75s@h:0.4, 74s@h:0.4, 65s@h:0.4, 64s@h:0.4, 54s@h:0.4, AJo-A4o@h:0.6, KJo-K8o@h:0.6",
  "AQs:0.8, AQo:0.8, KQs:0.8, KQo:0.8, QJs:0.8, QJo:0.7, QTs:0.7, QTo:0.6, Q9s:0.6, Q9o:0.5, Q7s-Q4s:0.4, 88:0.4, 33:0.4, 55:0.5, AJo:0.2, ATo:0.2, KJs:0.25, KTs:0.25, A2s+@h:0.5, KJs-K2s@h:0.5, J4s+@h:0.5, T6s+@h:0.5, 97s@h:0.5, 96s@h:0.5, 76s@h:0.5, 75s@h:0.5, 74s@h:0.5, 65s@h:0.5, 64s@h:0.5, 54s@h:0.5, AKo@h:0.6, AJo-A2o@h:0.6, KJo-K8o@h:0.5",
  "Monotone flop checked through, BTN stabbed the turn and BB called: Qx, a few slowplayed flushes and one-heart draws that missed. BTN stabbed with Qx, flushes and heart draws.")
r("btn_bb", "monotone", "bx",
  "QJs:0.6, QTs:0.6, Q9s:0.6, Q7s-Q6s:0.5, QJo:0.6, QTo:0.6, Q9o:0.6, KJs:0.5, KTs:0.5, K9s:0.5, KJo:0.5, KTo:0.5, K9o:0.5, KQo:0.4, A8s:0.4, 98s:0.4, 87s:0.4, 85s:0.5, 65s:0.4, 54s:0.4, 75s:0.4, AJo-A4o@h:0.4, 76s@h:0.6, 75s@h:0.6, 74s@h:0.6, 65s@h:0.6, 64s@h:0.6, 54s@h:0.6, 96s@h:0.6, 97s@h:0.6, T6s@h:0.5, T7s@h:0.5, J6s@h:0.5",
  "QJs:0.6, QTs:0.6, Q9s:0.6, Q7s-Q4s:0.5, QJo:0.6, QTo:0.6, Q9o:0.6, AQs:0.3, AQo:0.3, KQs:0.3, KQo:0.3, A8s:0.5, 98s:0.5, 87s:0.5, 86s:0.5, 85s:0.4, 98o:0.5, TT-99:0.5, 77-66:0.5, 44:0.5, 22:0.5, JJ:0.4, AA-KK:0.3, AJo:0.4, ATo:0.4, KJs:0.4, KTs:0.4, K9s:0.4, KJo:0.4, KTo:0.4, JTs:0.4, J9s:0.4, T9s:0.4, 76s:0.4, 65s:0.4, 54s:0.4, A2s+@h:0.3, KJs-K2s@h:0.3, J4s+@h:0.3, T6s+@h:0.3, 97s@h:0.3, 96s@h:0.3, 76s@h:0.3, 65s@h:0.3, 54s@h:0.3, AKo@h:0.6, AJo-A2o@h:0.6, KJo-K8o@h:0.5",
  "BB called a small c-bet on a monotone flop, the turn checked through, and BB blocks with Qx, paired kings and weak flushes. BTN checked back medium Qx, pairs and free-card heart draws.")
r("btn_bb", "monotone", "bb",
  "33:0.6, 88:0.6, 55:0.5, Q5s:0.6, KQo:0.6, QJs:0.2, ATs-A6s@h:0.8, KJs-K2s@h:0.8, JTs-J6s@h:0.8, T9s-T6s@h:0.7, 97s@h:0.7, 96s@h:0.7, 76s@h:0.6, 75s@h:0.6, 65s@h:0.6, 54s@h:0.6, 64s@h:0.6, 74s@h:0.5, AJo-A4o@h:0.5",
  "88:0.6, 33:0.6, 55:0.6, AA:0.5, KK:0.6, AQs:0.6, AQo:0.5, KQs:0.6, KQo:0.5, QJs:0.4, QJo:0.3, QTs:0.3, Q5s:0.5, A2s+@h, KJs-K2s@h, J4s+@h, T6s+@h, 97s@h, 96s@h, 76s@h, 75s@h, 74s@h, 65s@h, 64s@h, 54s@h, AKo@h:0.8, AJo-A2o@h:0.7, KJo-K8o@h:0.6",
  "BB called two barrels on a monotone board and leads big: flushes, sets and two pair against missed one-heart draws. BTN barrelled flushes, sets, top pair and the nut-flush draw.")

# ---------------- CO vs BTN ----------------
r("co_btn", "dry", "xx",
  "A9s-A6s:0.5, A4s:0.5, A2s:0.5, ATo-A9o:0.4, AJo:0.3, KQs:0.6, QJs:0.6, QTs:0.6, Q9s:0.6, KQo:0.5, QJo:0.5, QTo:0.5, TT-99:0.5, JJ:0.4, 77-66:0.4, 98s:0.5, 87s:0.5, 86s:0.5, 55:0.3, 44:0.4, 65s:0.4, 54s:0.4, KJs:0.4, KTs:0.4, K9s:0.3, JTs:0.3, T9s:0.3",
  "TT-99:0.7, 77-66:0.7, 44:0.7, 55:0.3, 88:0.3, 33:0.3, 22:0.3, A8s:0.3, AJs:0.4, ATs-A9s:0.5, AJo:0.4, KTs:0.8, QJs:0.8, QTs:0.8, JTs:0.8, J9s:0.8, T9s:0.8, 98s:0.7, 87s:0.7, KQo:0.8, 76s:0.4, 65s:0.4",
  "CO skipped the c-bet on an ace-high flop and both checked twice; CO's small river bet is thin value with weak aces, the river queen and pairs. BTN checked back weak pairs, weak aces and high cards.")
r("co_btn", "dry", "bx",
  "AKs, AKo, AQs, AQo, A5s:0.8, A3s:0.8, A8s, 88:0.8, 33:0.8, 55:0.6, QQ:0.8, KQs:0.3, AJs:0.3, AJo:0.3, KJs:0.5, KTs:0.5, K9s:0.5, K7s-K5s:0.4, JTs:0.4, J9s:0.4, T9s:0.4, 97s:0.4, 76s:0.4, 54s:0.5, 75s:0.3",
  "AJs:0.7, ATs:0.7, A9s:0.7, A8s:0.2, AJo:0.7, TT-99:0.7, 77-66:0.5, 44:0.4, 33:0.2, 88:0.2, 55:0.2, 22:0.3, 98s:0.6, 87s:0.6, QJs:0.2, QTs:0.2, JTs:0.2, J9s:0.2, T9s:0.2, KQo:0.2, KTs:0.2, 65s:0.2, 76s:0.2",
  "CO c-bet small, checked the turn and bets big on the river queen: top pair top kicker, two pair and sets against K-high and missed gutshots. BTN called with aces and pairs and checked back.")
r("co_btn", "dry", "bb",
  "AKs, AKo, AQs, AQo, AJs, AJo:0.8, ATs:0.8, ATo:0.6, A9s:0.6, A9o:0.4, A8s, A8o:0.5, A7s-A6s:0.4, A5s, A4s:0.6, A3s, A2s:0.6, 88, 33, 55:0.7, 76s:0.8, 75s:0.6, 65s:0.6, 54s:0.7, KQs:0.4, KQo:0.3, KJs:0.4, KTs:0.4, K9s:0.3, QJs:0.3, JTs:0.3, T9s:0.3",
  "AJs, ATs:0.9, A9s:0.7, A8s:0.6, AJo:0.9, 88:0.6, 33:0.6, 55:0.6, 44:0.4, TT:0.3, 99:0.3, 76s:0.6, 65s:0.4, QJs:0.1, KQo:0.1",
  "CO barrelled flop and turn; BTN called twice with aces, sets and the turned straight draw, which missed. CO holds strong aces, two pair, sets and draws.")
r("co_btn", "wet", "xx",
  "75s:0.6, 99:0.5, 88:0.5, 66:0.5, AA:0.5, A9s:0.6, A8s:0.5, A6s:0.5, A9o:0.4, A8o:0.4, JTs:0.8, QTs:0.6, QJs:0.4, KQs:0.3, KJs:0.3, 54s:0.6, KQs-K5s@d:0.6, QJs-Q8s@d:0.6, JTs-J8s@d:0.6, 76s@d:0.6, 65s@d:0.6, 54s@d:0.5",
  "AJs:0.6, ATs:0.6, A9s:0.7, A8s:0.7, AJo:0.6, JTs:0.7, QJs:0.4, QTs:0.4, KTs:0.4, KQo:0.3, J9s:0.7, T9s:0.7, 98s:0.5, 87s:0.7, 76s:0.6, 65s:0.5, TT:0.6, 77:0.5, 55:0.4, 44:0.3, KTs@d:0.8, QJs@d:0.8, QTs@d:0.8, JTs@d:0.8",
  "Flop checked through, BTN stabbed the turn and CO called; the river completes the diamonds. CO leads big with flushes, straights, sets and two pair against missed straight draws.")
r("co_btn", "wet", "bx",
  "KK-TT:0.5, AA:0.3, 99:0.3, 88:0.3, 66:0.3, 77:0.6, 55:0.4, 75s:0.3, AKs:0.4, AKo:0.4, AQs:0.4, AQo:0.4, AJs:0.4, AJo:0.4, ATs:0.4, ATo:0.4, A9s:0.6, A9o:0.6, A8s:0.6, A8o:0.6, K9s:0.6, Q9s:0.6, J9s:0.6, T9s:0.6, 98s:0.4, 87s:0.6, 86s:0.5, 76s:0.6, 65s:0.6, JTs:0.6, QTs:0.4, KTs:0.4, KQs:0.3, KJs:0.3, KQs-K5s@d:0.6, QJs-Q8s@d:0.6, JTs-J8s@d:0.6, 76s@d:0.6, 65s@d:0.6, 54s@d:0.5",
  "TT:0.6, 77:0.6, 55:0.4, 44:0.3, 99:0.2, 88:0.2, 66:0.2, A9s:0.6, A8s:0.6, J9s:0.6, T9s:0.6, 98s:0.3, 87s:0.6, 76s:0.6, 65s:0.4, JTs:0.7, QTs:0.4, QJs:0.3, KTs:0.3, AJs:0.4, ATs:0.4, AJo:0.3, KQo:0.2, KTs@d:0.6, QJs@d:0.6, QTs@d:0.6, JTs@d:0.6",
  "CO c-bet small and checked the turn: overpairs, top pair, draws and give-ups, some of which hit the river ace or flush. BTN called with pairs and draws and took the free card.")
r("co_btn", "wet", "bb",
  "KK-TT:0.5, AKs:0.5, AKo:0.4, AQs:0.4, AQo:0.4, A9s:0.6, A9o:0.6, A8s:0.6, A8o:0.6, A6s:0.6, J9s:0.4, T9s:0.4, K9s:0.4, Q9s:0.4, 99:0.3, 88:0.3, 66:0.3, 75s:0.3, JTs:0.3, QTs:0.3, 76s@d:0.7, 65s@d:0.7, 54s@d:0.5",
  "A9s:0.8, J9s:0.7, T9s:0.7, 98s:0.6, TT:0.6, 77:0.7, 87s:0.7, 76s:0.7, 65s:0.4, JTs:0.8, QJs:0.3, QTs:0.4, KTs:0.2, A8s:0.6, 99:0.3, 88:0.3, 66:0.3, KTs@d, QJs@d, QTs@d, JTs@d",
  "CO barrelled twice and blocks small on the flush-completing ace: overpairs, top pair and two pair plus weak flushes. BTN called two barrels with pairs and draws, the flush draws now made.")
r("co_btn", "paired", "xx",
  "AQs:0.2, KQs:0.2, QJs:0.2, QTs:0.2, Q9s:0.2, Q8s:0.2, AQo:0.2, KQo:0.2, QJo:0.2, QTo:0.2, JJ-TT:0.6, 99:0.4, 88-77:0.6, 66:0.2, 55-44:0.6, 33:0.3, 22:0.6, AKs:0.6, AKo:0.6, AJs-A2s:0.6, AJo-A8o:0.6, KJs-K5s:0.6, KJo:0.6, KTo:0.6, JTs:0.6, J9s:0.6, J8s:0.6, T9s:0.6, T8s:0.6, 98s:0.6, 97s:0.6, 87s:0.6, 86s:0.6, 76s:0.6, 75s:0.6, 65s:0.6, 54s:0.6, JTo:0.6, AA-KK:0.3",
  "TT:0.7, 99:0.5, 88-77:0.7, 66:0.2, 55-44:0.7, 33:0.2, 22:0.5, AJs-A8s:0.7, AJo:0.7, KTs:0.7, JTs:0.8, J9s:0.8, T9s:0.8, 98s:0.8, 87s:0.8, 76s:0.6, 65s:0.5, KQo:0.3, QJs:0.2, QTs:0.2",
  "CO checked a paired flop it would usually bet and both checked twice: wide, capped ranges of pairs and high cards with a few slowplayed queens.")
r("co_btn", "paired", "bx",
  "AA-KK:0.4, JJ-TT:0.6, AKs:0.5, AKo:0.5, AJs:0.4, AJo:0.4, 99:0.4, 66:0.3, 88-77:0.5, A9s:0.5, K9s:0.4, J9s:0.5, T9s:0.5, 98s:0.5, 97s:0.4, A6s:0.4, 65s:0.4, 76s:0.4, 86s:0.4, KQs:0.3, QJs:0.3, QTs:0.3, AQo:0.2, KJs:0.3, KTs:0.3",
  "TT:0.8, 99:0.4, 88-77:0.7, 66:0.3, 55-44:0.5, 33:0.3, 22:0.3, AJs-A8s:0.6, AJo:0.6, KQo:0.5, QJs:0.5, QTs:0.5, JTs:0.4, J9s:0.4, T9s:0.4, 98s:0.4, 87s:0.4, 76s:0.6, 65s:0.6, KTs:0.4",
  "CO c-bet the paired flop, checked the turn and blocks small with overpairs, river nines and A-high; BTN called with queens, pairs and A-high and checked back.")
r("co_btn", "paired", "bb",
  "AQs, AQo, KQs, KQo:0.8, QJs:0.8, QJo:0.6, QTs:0.6, QTo:0.4, Q9s, Q8s:0.5, AA:0.7, KK:0.6, 99, 66, 33, 54s:0.8, 75s:0.6, A5s:0.6, A4s:0.6, KJs:0.5, JTs:0.4, AKs:0.3, 87s:0.4, 65s:0.2, 76s:0.2",
  "TT:0.6, 99:0.6, 88-77:0.5, 66:0.2, 33:0.3, KQo:0.8, QJs:0.8, QTs:0.8, AJs:0.4, AJo:0.4, ATs:0.3, A9s:0.3, JTs:0.4, KTs:0.3, 76s:0.3, 65s:0.6, 98s:0.3, J9s:0.3, T9s:0.3",
  "CO fires a third barrel big: trips, full houses and overpairs against missed straight draws and A-high wheel draws. BTN called twice with queens, pairs and some draws.")
r("co_btn", "monotone", "xx",
  "KQs:0.5, KQo:0.5, KTs:0.6, KTo:0.6, K8s-K7s:0.5, KJs:0.3, KJo:0.3, K9s:0.4, A9s:0.4, T9s:0.4, 98s:0.4, 97s:0.4, QJs:0.4, JTs:0.4, J9s:0.3, AJs:0.4, AJo:0.4, AQs:0.4, AQo:0.4, ATs:0.3, T8s:0.4, 87s:0.4, 87s@s:0.6, 75s@s:0.6, 54s@s:0.6, T8s@s:0.6, J8s@s:0.5, Q8s@s:0.5",
  "KTs:0.8, KQo:0.8, AJs:0.6, AJo:0.6, JTs:0.6, J9s:0.6, T9s:0.6, 98s:0.5, 87s:0.5, TT-77:0.5, 55-44:0.5, QJs:0.6, QTs:0.6, A9s:0.5, A8s:0.3, ATs:0.3, A8s@s:0.6, ATs@s:0.6, AJs@s:0.6, KTs@s, QJs@s, QTs@s, JTs@s, 87s@s:0.5",
  "Monotone flop checked through, BTN stabbed the turn king and CO called; CO blocks small with Kx, river jacks and weak flushes. BTN holds Kx, pairs, flushes and the river straight.")
r("co_btn", "monotone", "bx",
  "QTs:0.7, QTo:0.7, KJs:0.6, KJo:0.6, K9s:0.6, 99:0.5, 66:0.5, 22:0.4, JJ:0.5, KK:0.5, AQs:0.5, AQo:0.5, ATs:0.4, ATo:0.4, T8s:0.5, 87s:0.6, 75s:0.5, 54s:0.4, A2s+@s:0.6, KQs-K5s@s:0.6, QJs-Q8s@s:0.6, JTs-J8s@s:0.6, T8s@s:0.6, 87s@s:0.6, 75s@s:0.6, 54s@s:0.6, AKo@s:0.6, AQo@s:0.8, A8o@s:0.6",
  "KTs:0.6, KQo:0.6, AJs:0.6, AJo:0.6, ATs:0.6, A9s:0.6, A8s:0.5, TT:0.6, 88-77:0.6, 55-44:0.5, J9s:0.6, T9s:0.7, 98s:0.6, 87s:0.6, QJs:0.5, QTs:0.5, JTs:0.5, A8s@s:0.6, ATs@s:0.6, AJs@s:0.6, KTs@s:0.5, QJs@s:0.5, QTs@s:0.5, JTs@s:0.5, 87s@s:0.4",
  "CO c-bet the monotone flop, checked the turn and bets big: flushes, the straight, sets and two pair against missed one-spade draws. BTN called and checked back pairs, Kx and some flushes.")
r("co_btn", "monotone", "bb",
  "AKs:0.8, AKo:0.8, KQs:0.7, KQo:0.7, KJs:0.6, KJo:0.6, K9s:0.6, 99, 66, 22:0.8, KK, AA:0.5, QTs:0.3, QJs:0.3, JTs:0.3, T9s:0.3, A2s+@s, KQs-K5s@s, QJs-Q8s@s, JTs-J8s@s, T8s@s, 87s@s, 75s@s, 54s@s, AQo@s:0.6, AJo@s:0.6, ATo@s:0.6, A9o@s:0.6, A8o@s:0.6",
  "KTs:0.8, KQo:0.8, A9s:0.3, T9s:0.3, J9s:0.3, 98s:0.3, AJs:0.3, ATs:0.3, TT:0.3, 88-77:0.2, QJs:0.2, QTs:0.4, 55-44:0.1, A8s@s, ATs@s, AJs@s, KTs@s, QJs@s, QTs@s, JTs@s, 87s@s, AJo@s:0.6",
  "CO barrelled the monotone flop and the turn king; BTN called with Kx, flushes and spade draws. CO holds flushes, sets, Kx and ace-of-spades draws that missed.")

# ---------------- SB 3-bet vs BTN ----------------
r("sb_btn_3bet", "dry", "xx",
  "AKs:0.6, AKo:0.6, KQs:0.6, KJs:0.6, KTs:0.4, KJo:0.4, KK:0.5, 88:0.5, AA:0.6, QQ:0.2, A5s:0.7, A4s:0.7, QJs:0.5, QTs:0.5, JTs:0.5, T9s:0.4, A9s:0.3, ATs:0.3, AJs:0.3",
  "QQ:0.15, JJ:0.7, TT-99:0.7, 77:0.7, 66:0.3, 55:0.3, AQs:0.7, AQo:0.7, AJs:0.7, AJo:0.35, ATs:0.7, A9s:0.3, KJs:0.4, KTs:0.3, KQs:0.3, KQo:0.15, QJs:0.6, QTs:0.3, JTs:0.6, T9s:0.6, 98s:0.3, AKs:0.06, AKo:0.06",
  "The 3-bettor checked a dry flop and both checked twice; SB leads big with slowplayed Kx, overpairs and sets against missed wheel and straight draws. BTN checked back pairs and A-high.")
r("sb_btn_3bet", "dry", "bx",
  "AA:0.4, KK:0.2, QQ-99:0.6, 88:0.2, AKs:0.3, AKo:0.3, KQs:0.4, KJs:0.5, KTs:0.6, KJo:0.5, AQs:0.6, AQo:0.6, AJs:0.6, AJo:0.6, ATs:0.6, ATo:0.6, A9s:0.6, A8s:0.6, A5s-A2s:0.5, QJs:0.5, QTs:0.5, JTs:0.5, T9s:0.6",
  "KQs:0.6, KQo:0.3, KJs:0.7, KTs:0.6, AKs:0.06, AKo:0.06, JJ-99:0.6, 77:0.6, 66:0.6, 55:0.2, 88:0.3, AQs:0.4, AQo:0.4, AJs:0.3, ATs:0.3, A9s:0.15, QJs:0.2, JTs:0.2, T9s:0.4, 98s:0.25",
  "SB c-bet small and checked the turn: overpairs, A-high and some Kx. BTN called with Kx, pairs and a few floats and checked back.")
r("sb_btn_3bet", "dry", "bb",
  "AKs:0.5, AKo:0.5, KQs:0.5, KJs:0.5, KJo:0.5, AA:0.5, QQ:0.4, JJ:0.3, KK:0.3, 88:0.3, KTs:0.5, A5s:0.4, A4s:0.4, QJs:0.3, T9s:0.3",
  "KQs:0.9, KQo:0.45, KJs:0.9, KTs:0.63, AKs:0.2, AKo:0.2, 88:0.6, 66:0.6, JJ-99:0.3, 77:0.3, T9s:0.2, 98s:0.2",
  "SB barrelled twice and blocks small with top pair and overpairs plus a few wheel-draw bluffs; BTN called with Kx, sets and some pairs.")
r("sb_btn_3bet", "wet", "xx",
  "AA:0.8, KK:0.8, QQ:0.8, JJ:0.8, 88:0.7, T9s:0.7, ATs:0.6, ATo:0.5, KTs:0.6, QTs:0.6, JTs:0.7, A9s:0.5, A8s:0.7, QJs:0.5, KJs:0.3, AKs:0.2, AKo:0.2, AQo:0.2, AKs@h:0.6, AQs@h:0.6, AJs@h:0.6, A5s@h:0.5, KQs@h:0.6, KJs@h:0.6",
  "98s:0.4, 88:0.6, 77:0.4, 66:0.5, TT:0.3, 99:0.3, JJ:0.6, QJs:0.7, JTs:0.6, T9s:0.5, ATs:0.6, KTs:0.35, QTs:0.25, A9s:0.2, KQs:0.3, AQs:0.3, 55:0.15, AQs@h:0.8, AJs@h:0.8, KQs@h:0.8, KJs@h:0.8, QJs@h:0.8, AKs@h:0.2, AQo@h:0.5",
  "Flop checked through, BTN stabbed the turn six and SB called: overpairs, Tx, straights and heart draws that missed. BTN stabbed with straights, sets, pairs and draws.")
r("sb_btn_3bet", "wet", "bx",
  "QQ:0.5, JJ:0.6, KK:0.4, AA:0.4, ATs:0.5, ATo:0.5, KTs:0.5, QTs:0.5, JTs:0.5, A9s:0.5, T9s:0.4, 88:0.3, KQs:0.3, AKo:0.2, A8s:0.4, AKs@h:0.4, AQs@h:0.4, A5s@h:0.4",
  "JJ:0.6, TT:0.3, 99:0.3, 77:0.3, 66:0.3, 88:0.4, 98s:0.25, QJs:0.6, JTs:0.7, T9s:0.4, ATs:0.6, A9s:0.25, KTs:0.4, QTs:0.3, KJs:0.3, AJs:0.3, 55:0.15, AQs@h:0.6, AJs@h:0.6, KQs@h:0.6, KJs@h:0.6, QJs@h:0.6, AKs@h:0.2",
  "SB c-bet small and checked the turn; its small river bet is overpairs and Tx with a few missed heart draws. BTN called with pairs and draws and checked back.")
r("sb_btn_3bet", "wet", "bb",
  "88, A8s, TT:0.8, 99:0.8, T9s:0.8, AA:0.3, KK:0.3, QJs:0.5, AKs:0.2, KQs:0.2, AKs@h:0.8, AQs@h:0.8, AJs@h:0.8, KQs@h:0.8, KJs@h:0.8, A5s@h:0.5",
  "98s:0.3, 88:0.6, 77:0.4, 66:0.5, 99:0.4, TT:0.4, JJ:0.4, T9s:0.6, QJs:0.6, JTs:0.6, ATs:0.5, KTs:0.3, QTs:0.2, A9s:0.15, 55:0.15, AQs@h:0.8, AJs@h:0.8, KQs@h:0.8, KJs@h:0.8, QJs@h:0.8, AKs@h:0.25",
  "SB fires a third barrel big: straights, sets and two pair against missed heart and straight draws. BTN called twice with straights, sets, pairs and draws.")
r("sb_btn_3bet", "paired", "xx",
  "AKs:0.4, AKo:0.4, KQs:0.5, KJs:0.5, KTs:0.5, KJo:0.5, QQ:0.4, JJ:0.4, TT:0.4, 99:0.4, AA:0.3, A5s:0.4, A4s:0.4, A3s:0.3, AQs:0.4, AQo:0.3, AJs:0.4, ATs:0.3, QJs:0.4, JTs:0.3, T9s:0.3",
  "KQs:0.4, KJs:0.4, KTs:0.35, KQo:0.2, JJ-99:0.6, 77-66:0.6, 55:0.25, QQ:0.12, AQs:0.6, AQo:0.6, AJs:0.6, AJo:0.3, ATs:0.6, A9s:0.3, QJs:0.6, QTs:0.3, JTs:0.6, T9s:0.6, 98s:0.1, 88:0.3, AKs:0.06, AKo:0.06",
  "SB checked a paired flop and both checked twice; SB blocks small with turned Kx and pocket pairs. BTN checked back pairs and A-high.")
r("sb_btn_3bet", "paired", "bx",
  "A8s, AKs:0.6, AKo:0.6, KQs:0.5, AA:0.6, KK:0.6, A5s:0.6, A4s:0.6, QJs:0.4, JTs:0.4, T9s:0.4, AQs:0.3, AQo:0.3, KJs:0.3",
  "JJ-99:0.7, 77-66:0.7, 55:0.25, QQ:0.18, AQs:0.5, AQo:0.5, AJs:0.4, AJo:0.2, ATs:0.4, A9s:0.2, KQs:0.4, KQo:0.2, KJs:0.4, KTs:0.3, T9s:0.4, 98s:0.35, QJs:0.3, JTs:0.3, AKs:0.06, AKo:0.06",
  "SB c-bet the paired flop, checked the turn king and bets big: trips, sets and strong Kx against wheel draws and high cards. BTN called with pairs and A-high and checked back.")
r("sb_btn_3bet", "paired", "bb",
  "AA, KK, QQ:0.6, JJ:0.5, AKs, AKo, KQs, KJs:0.8, KTs:0.6, KJo:0.6, A8s, A5s:0.6, A4s:0.6, QJs:0.4, JTs:0.4, T9s:0.4, AQs:0.4, AQo:0.3, TT:0.4, 99:0.3",
  "KQs, KQo:0.5, KJs, KTs:0.7, AKs:0.2, AKo:0.2, 98s:0.4, QQ:0.15, JJ:0.4, TT-99:0.3, 77:0.2, AQs:0.3, AQo:0.2",
  "SB barrelled the paired flop and the turn king; BTN called with Kx, trips and some pairs.")
r("sb_btn_3bet", "monotone", "xx",
  "KK:0.6, 99:0.6, AA:0.3, AKs:0.3, AKo@d:0.4, AQo@d:0.6, AJo@d:0.6, ATo@d:0.6, A8s@d:0.8, A4s-A2s@d:0.8, AQs@d:0.8, AJs@d:0.8, ATs@d:0.8, QJs@d:0.8, QTs@d:0.8, JTs@d:0.8",
  "KQs:0.7, KQo:0.35, KJs:0.7, KTs:0.5, AKs:0.12, AKo:0.12, A9s:0.25, 98s:0.25, JJ-TT:0.5, 99:0.4, 88:0.5, 77:0.4, 66:0.5, 55:0.2, QQ:0.15, AQs@d, AJs@d, ATs@d, QJs@d, QTs@d:0.5, JTs@d, AQo@d:0.6, AJo@d:0.25",
  "Monotone flop checked through, BTN stabbed the turn and SB called; SB leads big with flushes and sets against missed ace-of-diamonds draws. BTN holds Kx, pairs and flushes.")
r("sb_btn_3bet", "monotone", "bx",
  "AA:0.5, KK:0.3, QQ-TT:0.6, 99:0.2, 88:0.6, AKs:0.6, AKo:0.6, KQs:0.6, KJs:0.6, KTs:0.6, KJo:0.6, AQs:0.5, AJs:0.5, ATs:0.5, A8s:0.5, A9s:0.5, QJs:0.4, JTs:0.4, T9s:0.5, AQo@d:0.6, AJo@d:0.6, ATo@d:0.6, AQs@d:0.4, AJs@d:0.4, ATs@d:0.4, A8s@d:0.4, A4s-A2s@d:0.4, QJs@d:0.4, QTs@d:0.4, JTs@d:0.4",
  "KQs:0.6, KQo:0.3, KJs:0.6, KTs:0.4, AKs:0.1, AKo:0.1, JJ-TT:0.6, 88:0.6, 77:0.6, 66:0.6, 99:0.3, 55:0.15, A9s:0.3, T9s:0.5, 98s:0.25, QQ:0.15, AQs@d:0.5, AJs@d:0.5, ATs@d:0.5, QJs@d:0.5, QTs@d:0.25, JTs@d:0.5, AQo@d:0.6, AJo@d:0.25",
  "SB c-bet the monotone flop and checked the turn: Kx, overpairs, some flushes and ace-of-diamonds draws. BTN called and checked back.")
r("sb_btn_3bet", "monotone", "bb",
  "AKs:0.6, AKo:0.6, KQs:0.6, KJs:0.5, KTs:0.4, AA:0.5, KK:0.3, 99:0.3, AQo@d:0.2, A8s@d:0.4, A4s-A2s@d:0.4, QTs@d:0.5, JTs@d:0.5",
  "KQs:0.8, KQo:0.4, KJs:0.8, KTs:0.56, AKs:0.15, AKo:0.15, 99:0.5, 77:0.2, QQ:0.1, A9s:0.15, T9s:0.2, AQs@d, AJs@d, ATs@d, QJs@d, QTs@d:0.5, JTs@d, AQo@d:0.6, AJo@d:0.25",
  "SB barrelled twice on a monotone board and blocks small with top pair and weak flushes; BTN called with Kx, flushes and the nut-flush draw.")

def role_of(pot_i, fam_i, line_i):
    return ROLES[(fam_i + line_i + pot_i) % 3]

def build():
    spots = []
    for pi, pot in enumerate(POTS):
        P = POTS[pot]
        O, I = P["oop"], P["ip"]
        agg = O if AGGRESSOR[pot] == "oop" else I
        for fi, fam in enumerate(FAMILIES):
            for li, line in enumerate(LINES):
                if line == "xx":
                    flop = f"{O}:X {I}:X"
                    turn = f"{O}:X {I}:X" if fam in ("dry", "paired") else f"{O}:X {I}:B50% {O}:C"
                else:
                    flop = f"{agg}:B33% {I}:C" if agg == O else f"{O}:X {I}:B33% {O}:C"
                    if line == "bx":
                        turn = f"{O}:X {I}:X"
                    else:
                        tb = P["turn_bet"]
                        turn = f"{agg}:B{tb} {I}:C" if agg == O else f"{O}:X {I}:B{tb} {O}:C"
                role = role_of(pi, fi, li)
                river = "" if role == "oop" else f"{O}:B{role[2:]}%"
                oop_range, ip_range, note = R[(pot, fam, line)]
                line_text = {"xx": "checked to the river" if fam in ("dry", "paired") else "flop checked, turn bet-call",
                             "bx": "flop bet-call, turn checked", "bb": "flop and turn bet-call"}[line]
                hero_text = "hero OOP first to act" if role == "oop" else f"hero IP facing a {role[2:]}% bet"
                spot = {
                    "id": f"{pot}-{fam}-{line}",
                    "description": f"{P['name']}; {fam} board, {line_text}; {hero_text}. {note}",
                    "pot_type": pot, "family": fam, "line": line,
                    "board": BOARDS[(pot, fam)],
                    "pre": P["pre"], "flop": flop, "turn": turn, "river": river,
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
    for street in ["pre", "flop", "turn", "river"]:
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
doc = {
    "format": "See tools/bench/README.md. Ranges are assumed, not solved: each is the player's range at the hero's river decision.",
    "stack": 2000, "small_blind": 10, "big_blind": 20,
    "spots": spots,
}
out = sys.argv[1]
with open(out, "w") as f:
    f.write("{\n")
    f.write('  "format": ' + json.dumps(doc["format"]) + ",\n")
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
print(Counter((s["pot_type"], s["hero"] + s["river"][-4:]) for s in spots))
