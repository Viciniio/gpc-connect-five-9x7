#!/usr/bin/env python3
"""Reproduce the cross-board checks reported for GPC Phase 2."""
import json, random, time
from gpc_phase2 import Game, enumerate_legal_states, column_theorem_safe_moves, naive_safe_moves

results={"format":"GPC-EXTENDED-AUDIT-1"}
# Exhaustive symmetry-representative prefix on a second board/rule regime.
g=Game.make(4,4,4); t=time.time(); states=0; checked=0
for s in enumerate_legal_states(g,max_ply=10):
    states += 1
    if not g.terminal(s):
        fast=set(column_theorem_safe_moves(g,s))
        naive=set(naive_safe_moves(g,s))
        if fast != naive:
            raise AssertionError({"board":"4x4 connect4","state":s,"fast":fast,"naive":naive})
        checked += 1
results["4x4_connect4_prefix"]={"scope":"all symmetry-canonical legal states through ply 10",
    "states_including_terminal":states,"nonterminal_comparisons":checked,
    "result":"all fast-vs-naive response sets matched","seconds":round(time.time()-t,3)}
# Deterministic sample of complete legal trajectories on the target board.
rng=random.Random(20261109); g=Game.make(9,7,5); total=0; per_game=[]
t=time.time()
for game_index in range(250):
    s=(0,0,g.empty_heights,0); n=0
    while not g.terminal(s):
        fast=set(column_theorem_safe_moves(g,s))
        naive=set(naive_safe_moves(g,s))
        if fast != naive:
            raise AssertionError({"board":"9x7 connect5","game":game_index,"state":s,"fast":fast,"naive":naive})
        n += 1; total += 1
        s=g.play(s,rng.choice(g.legal_moves(s)))
    per_game.append(n)
results["9x7_connect5_random_trajectories"]={"seed":20261109,"sampling":"uniform random legal column at each turn; stop on first win/full board",
    "games":250,"nonterminal_state_visits_compared":total,
    "minimum_visits_per_game":min(per_game),"maximum_visits_per_game":max(per_game),
    "result":"all fast-vs-naive response sets matched","seconds":round(time.time()-t,3)}
print(json.dumps(results,indent=2))
