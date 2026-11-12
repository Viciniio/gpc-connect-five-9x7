#!/usr/bin/env python3
"""Turn exact settled DFPN memo records into an independently checked GPC-ANDOR certificate."""
from __future__ import annotations
import argparse, hashlib, json, sys
from pathlib import Path
from typing import Dict, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PHASE2 = ROOT / "phase2"
if str(PHASE2) not in sys.path:
    sys.path.insert(0, str(PHASE2))
from gpc_phase2 import (
    Game, encode_state, verify_certificate, immediate_winning_columns,
    tactical_profile, find_compound_fork_move, find_same_column_ladder,
)

W, H, STRIDE = 9, 7, 8


def sentinel_to_state(pos: int, mask: int):
    total = mask.bit_count()
    side = total & 1
    p0 = p1 = 0
    heights = []
    for c in range(W):
        height = 0
        for r in range(H):
            src = 1 << (c * STRIDE + r)
            dst = 1 << (c * H + r)
            if mask & src:
                height += 1
                if pos & src:
                    if side == 0: p0 |= dst
                    else: p1 |= dst
                else:
                    if side == 0: p1 |= dst
                    else: p0 |= dst
        heights.append(height)
    return p0, p1, tuple(heights), side


def load_solved(path: Path, game: Game):
    table = {}
    if not path.exists():
        return table
    with path.open("r", encoding="ascii") as f:
        for line_no, line in enumerate(f, 1):
            parts = line.split()
            if not parts:
                continue
            if len(parts) != 9:
                raise ValueError(f"bad solved record on line {line_no}: expected 9 fields")
            plo, phi, mlo, mhi, pn, dn, best, frontier, remaining = map(int, parts)
            pos = (phi << 64) | plo
            mask = (mhi << 64) | mlo
            if pos & ~mask:
                raise ValueError(f"invalid occupancy in solved record {line_no}")
            if int(frontier) and (pn == 0 or dn == 0):
                raise ValueError(f"frontier incorrectly marked solved on line {line_no}")
            state = sentinel_to_state(pos, mask)
            key = game.canonical(state)
            old = table.get(key)
            # The settled result is exact. If duplicate orientations occur, they must agree.
            val = (pn, dn)
            if old is not None and old != val:
                raise ValueError(f"conflicting settled values at line {line_no}: {old} vs {val}")
            table[key] = val
    return table


def build_certificate(game: Game, root, history, target: int, claim: str, table: Dict):
    if claim not in ("target_win", "target_not_win"):
        raise ValueError("invalid claim")
    key0 = game.canonical(root)
    root_value = table.get(key0)
    if root_value is None:
        raise ValueError("root position is missing from settled DFPN memo")
    if claim == "target_win" and root_value[0] != 0:
        raise ValueError("DFPN root has not proved target_win")
    if claim == "target_not_win" and root_value[1] != 0:
        raise ValueError("DFPN root has not refuted target_win")

    nodes = []
    ids = {}

    def rec(s):
        cs = game.canonical(s)
        memo_key = (cs, target, claim)
        if memo_key in ids:
            return ids[memo_key]
        nid = len(nodes)
        ids[memo_key] = nid
        nodes.append(None)
        winner = game.winner(s)
        terminal = winner is not None or all(h == game.height for h in s[2])
        if terminal:
            ok = (winner == target) if claim == "target_win" else (winner != target)
            if not ok:
                raise ValueError(f"terminal state contradicts {claim}: winner={winner}, target={target}")
            nodes[nid] = {"id": nid, "kind": "terminal", "state": encode_state(s),
                          "winner": winner, "claim": claim}
            return nid

        side = s[3]
        moves = list(game.legal_moves(s))

        # DFPN safely reduces a target-to-move position with one opponent threat
        # to the forced block. Other target moves are pruned because they allow
        # an immediate counter-win; the independent certificate must nevertheless
        # cover every legal target move. Reconstruct those omitted branches as a
        # one-move OR_NOT_WIN counter-witness before consulting the memo table.
        if claim == "target_win" and side == target:
            # The DFPN opponent-turn reduction explores only the forced block
            # when the target already has one immediate winning column. Every
            # non-block reply is still part of the AND certificate, and it hands
            # the target an immediate win. Reconstruct that leaf directly.
            direct_target_wins = list(immediate_winning_columns(game, s, target))
            if direct_target_wins:
                nodes[nid] = {"id": nid, "kind": "macro_immediate_win", "state": encode_state(s),
                              "target": target, "winning_columns": direct_target_wins, "claim": claim}
                return nid

        if claim == "target_not_win" and side != target:
            direct_defender_wins = list(immediate_winning_columns(game, s, side))
            if direct_defender_wins:
                c = direct_defender_wins[0]
                nodes[nid] = {"id": nid, "kind": "OR_NOT_WIN", "state": encode_state(s),
                              "target": target, "move": c,
                              "child": rec(game.play(s, c)), "claim": claim}
                return nid

        value = table.get(cs)
        required_index = 0 if claim == "target_win" else 1
        if value is None or value[required_index] != 0:
            raise ValueError(f"certificate frontier: required settled state missing at ply {sum(s[2])}")

        if claim == "target_win":
            if side == target:
                wins = list(immediate_winning_columns(game, s, target))
                if wins:
                    nodes[nid] = {"id": nid, "kind": "macro_immediate_win", "state": encode_state(s),
                                  "target": target, "winning_columns": wins, "claim": claim}
                    return nid
                fork = find_compound_fork_move(game, s, target)
                if fork is not None:
                    nodes[nid] = {"id": nid, "kind": "macro_compound_fork", "state": encode_state(s),
                                  "target": target, "move": fork["move"],
                                  "winning_columns": fork["winning_columns"], "claim": claim}
                    return nid
                choices = []
                for c in moves:
                    child = game.play(s, c)
                    child_winner = game.winner(child)
                    if game.terminal(child) and child_winner == target:
                        choices.append(c)
                        break
                    child_value = table.get(game.canonical(child))
                    if child_value is not None and child_value[0] == 0:
                        choices.append(c)
                        break
                if not choices:
                    raise ValueError("proved OR node has no settled winning child")
                c = choices[0]
                nodes[nid] = {"id": nid, "kind": "OR", "state": encode_state(s),
                              "target": target, "move": c,
                              "child": rec(game.play(s, c)), "claim": claim}
            else:
                target_wins = list(immediate_winning_columns(game, s, target))
                defender_wins = list(immediate_winning_columns(game, s, side))
                if not defender_wins and len(target_wins) >= 2:
                    nodes[nid] = {"id": nid, "kind": "macro_double_threat", "state": encode_state(s),
                                  "target": target, "winning_columns": target_wins, "claim": claim}
                    return nid
                edges = []
                for c in moves:
                    child = game.play(s, c)
                    child_winner = game.winner(child)
                    if game.terminal(child):
                        child_id = rec(child)
                    else:
                        child_value = table.get(game.canonical(child))
                        if child_value is None or child_value[0] != 0:
                            raise ValueError(f"AND node is missing target-win reply {c}")
                        child_id = rec(child)
                    edges.append({"move": c, "child": child_id})
                nodes[nid] = {"id": nid, "kind": "AND", "state": encode_state(s),
                              "target": target, "children": edges, "claim": claim}
        else:
            if side == target:
                own_now = list(immediate_winning_columns(game, s, target))
                enemy_now = list(immediate_winning_columns(game, s, 1 - target))
                if not own_now and len(enemy_now) >= 2:
                    nodes[nid] = {"id": nid, "kind": "macro_double_opponent_threats", "state": encode_state(s),
                                  "target": target, "winning_columns": enemy_now, "claim": claim}
                    return nid
                ladder = find_same_column_ladder(game, s, target)
                if ladder is not None:
                    nodes[nid] = {"id": nid, "kind": "macro_same_column_ladder", "state": encode_state(s),
                                  "target": target, **ladder, "claim": claim}
                    return nid
                profile = tactical_profile(game, s)
                if not own_now and not profile["safe_moves"]:
                    nodes[nid] = {"id": nid, "kind": "macro_no_safe_reply", "state": encode_state(s),
                                  "target": target, "claim": claim}
                    return nid
                edges = []
                for c in moves:
                    child = game.play(s, c)
                    child_winner = game.winner(child)
                    if game.terminal(child):
                        child_id = rec(child)
                    else:
                        child_value = table.get(game.canonical(child))
                        if child_value is None or child_value[1] != 0:
                            raise ValueError(f"AND_NOT_WIN node is missing target move {c}")
                        child_id = rec(child)
                    edges.append({"move": c, "child": child_id})
                nodes[nid] = {"id": nid, "kind": "AND_NOT_WIN", "state": encode_state(s),
                              "target": target, "children": edges, "claim": claim}
            else:
                chosen = None
                for c in moves:
                    child = game.play(s, c)
                    child_winner = game.winner(child)
                    if game.terminal(child) and child_winner != target:
                        chosen = c
                        break
                    child_value = table.get(game.canonical(child))
                    if child_value is not None and child_value[1] == 0:
                        chosen = c
                        break
                if chosen is None:
                    raise ValueError("proved OR_NOT_WIN node has no defender counter-move")
                nodes[nid] = {"id": nid, "kind": "OR_NOT_WIN", "state": encode_state(s),
                              "target": target, "move": chosen,
                              "child": rec(game.play(s, chosen)), "claim": claim}
        return nid

    root_id = rec(root)
    cert = {"format": "GPC-ANDOR-CERT-1", "game": {"width": W, "height": H, "k": 5},
            "root": root_id, "target": target, "claim": claim,
            "root_history": list(history), "nodes": nodes}
    cert["sha256"] = hashlib.sha256(json.dumps(cert, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    ok, message = verify_certificate(cert)
    if not ok:
        raise ValueError("independent certificate verifier rejected result: " + message)
    return cert, message


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--solved", required=True, type=Path)
    ap.add_argument("--target", type=int, choices=(0, 1), required=True)
    ap.add_argument("--claim", choices=("target_win", "target_not_win"), required=True)
    ap.add_argument("--history", nargs=2, type=int, required=True, help="first and second columns, 0..8")
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    game = Game.make(W, H, 5)
    history = list(args.history)
    root = game.legal_history(history)
    table = load_solved(args.solved, game)
    cert, message = build_certificate(game, root, history, args.target, args.claim, table)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(cert, separators=(",", ":")), encoding="utf-8")
    print(f"PASS {args.claim}: {message}; target={args.target}; root_history={history}")

if __name__ == "__main__":
    main()
