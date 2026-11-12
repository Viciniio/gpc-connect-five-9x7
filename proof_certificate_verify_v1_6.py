#!/usr/bin/env python3
"""Independent verifier for gravity Connect-5 strategy certificates (9x7).

Supports the original expanded AND/OR tree and the compact state-bound DAG
format emitted by the persistent native Gravity-LDFPN watcher. This module uses
an independent byte-board representation and never trusts search PN/DN values.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path

W, H, K = 9, 7, 5
DIRS = ((1, 0), (0, 1), (1, 1), (1, -1))


def won(board, side):
    for y in range(H):
        for x in range(W):
            if board[y][x] != side:
                continue
            for dx, dy in DIRS:
                ex, ey = x + 4 * dx, y + 4 * dy
                if 0 <= ex < W and 0 <= ey < H and all(
                    board[y + i * dy][x + i * dx] == side for i in range(5)
                ):
                    return True
    return False


def replay(history):
    b = [[0] * W for _ in range(H)]
    h = [0] * W
    winner = None
    for ply, c in enumerate(history):
        if isinstance(c, bool) or not isinstance(c, int) or not 0 <= c < W or h[c] >= H:
            raise ValueError(f"illegal history move at ply {ply}: {c}")
        side = 1 + (ply & 1)
        y = h[c]
        b[y][c] = side
        h[c] += 1
        if won(b, side):
            winner = side
            if ply != len(history) - 1:
                raise ValueError("history continues after a win")
    return b, h, 1 + (len(history) & 1), winner


def state_key(board, side_to_move):
    p0 = p1 = 0
    for y in range(H):
        for x in range(W):
            bit = 1 << (y * W + x)
            if board[y][x] == 1:
                p0 |= bit
            elif board[y][x] == 2:
                p1 |= bit
    return f"{p0:016x}:{p1:016x}:{side_to_move - 1}"


class ExpandedChecker:
    def __init__(self, attacker, max_nodes):
        self.attacker = 1 if attacker == "X" else 2
        self.max_nodes = max_nodes
        self.nodes = 0
        self.max_depth = 0

    def visit(self, node, b, h, side, previous_winner, depth):
        self.nodes += 1
        self.max_depth = max(self.max_depth, depth)
        if self.nodes > self.max_nodes:
            raise ValueError("certificate exceeds node limit")
        if previous_winner == self.attacker:
            if not isinstance(node, dict) or node.get("type") != "WIN":
                raise ValueError("attacker terminal branch must end with a WIN leaf")
            return
        if previous_winner is not None:
            raise ValueError("certificate branch ended with defender win")
        if not isinstance(node, dict) or node.get("type") == "WIN":
            raise ValueError("WIN leaf supplied before attacker has five")
        legal = [c for c in range(W) if h[c] < H]
        if not legal:
            raise ValueError("full-board draw cannot prove attacker win")
        expected = "OR" if side == self.attacker else "AND"
        if node.get("type") != expected:
            raise ValueError(f"expected {expected} node for {'X' if side == 1 else 'O'} to move")
        if expected == "OR":
            c = node.get("move")
            if isinstance(c, bool) or not isinstance(c, int) or c not in legal:
                raise ValueError("OR node has an illegal move")
            nb = [row[:] for row in b]
            nh = h[:]
            y = nh[c]
            nb[y][c] = side
            nh[c] += 1
            win = side if won(nb, side) else None
            self.visit(node.get("child"), nb, nh, side ^ 3, win, depth + 1)
        else:
            children = node.get("children")
            if not isinstance(children, list):
                raise ValueError("AND node must contain a children list")
            cols = []
            for item in children:
                if not isinstance(item, dict):
                    raise ValueError("AND child entry must be an object")
                c = item.get("move")
                if isinstance(c, bool) or not isinstance(c, int) or c not in legal:
                    raise ValueError("AND node contains an illegal defender reply")
                cols.append(c)
            if len(cols) != len(set(cols)) or set(cols) != set(legal):
                raise ValueError(f"AND node must cover every legal defender reply exactly once; expected {legal}, got {cols}")
            for item in children:
                c = item["move"]
                nb = [row[:] for row in b]
                nh = h[:]
                y = nh[c]
                nb[y][c] = side
                nh[c] += 1
                win = side if won(nb, side) else None
                self.visit(item.get("node"), nb, nh, side ^ 3, win, depth + 1)


class DAGChecker:
    def __init__(self, attacker, nodes, max_nodes):
        self.attacker = 1 if attacker == "X" else 2
        self.nodes_by_id = nodes
        self.max_nodes = max_nodes
        self.visits = 0
        self.max_depth = 0
        self.bound_state = {}
        self.verified_ids = set()
        self.active = set()

    def visit(self, node_id, b, h, side, previous_winner, depth):
        self.visits += 1
        self.max_depth = max(self.max_depth, depth)
        if self.visits > self.max_nodes:
            raise ValueError("certificate exceeds node/visit limit")
        if isinstance(node_id, bool) or not isinstance(node_id, int) or node_id not in self.nodes_by_id:
            raise ValueError("DAG contains an invalid node reference")
        node = self.nodes_by_id[node_id]
        expected_key = state_key(b, side)
        if node.get("state") != expected_key:
            raise ValueError(f"state binding mismatch at DAG node {node_id}")
        prior_key = self.bound_state.setdefault(node_id, expected_key)
        if prior_key != expected_key:
            raise ValueError("one DAG node was reused for two different game states")
        if node_id in self.active:
            raise ValueError("cycle detected in strategy DAG")
        if node_id in self.verified_ids:
            return
        if previous_winner == self.attacker:
            if node.get("type") != "WIN":
                raise ValueError("attacker terminal branch must end with WIN")
            self.verified_ids.add(node_id)
            return
        if previous_winner is not None:
            raise ValueError("strategy branch ends with defender win")
        if node.get("type") == "WIN":
            raise ValueError("WIN node before attacker has five")
        legal = [c for c in range(W) if h[c] < H]
        if not legal:
            raise ValueError("draw cannot certify a forced win")
        expected_type = "OR" if side == self.attacker else "AND"
        if node.get("type") != expected_type:
            raise ValueError(f"expected {expected_type} node at {node_id}")
        self.active.add(node_id)
        try:
            if expected_type == "OR":
                c = node.get("move")
                if isinstance(c, bool) or not isinstance(c, int) or c not in legal:
                    raise ValueError("OR node has illegal attacker move")
                nb = [row[:] for row in b]
                nh = h[:]
                y = nh[c]
                nb[y][c] = side
                nh[c] += 1
                winner = side if won(nb, side) else None
                self.visit(node.get("child"), nb, nh, side ^ 3, winner, depth + 1)
            else:
                children = node.get("children")
                if not isinstance(children, list):
                    raise ValueError("AND node must contain children")
                cols = []
                for item in children:
                    if not isinstance(item, dict):
                        raise ValueError("AND edge must be an object")
                    c = item.get("move")
                    if isinstance(c, bool) or not isinstance(c, int) or c not in legal:
                        raise ValueError("AND node contains illegal defender reply")
                    cols.append(c)
                if len(cols) != len(set(cols)) or set(cols) != set(legal):
                    raise ValueError(f"AND node must cover every legal reply exactly once; expected {legal}, got {cols}")
                for item in children:
                    c = item["move"]
                    nb = [row[:] for row in b]
                    nh = h[:]
                    y = nh[c]
                    nb[y][c] = side
                    nh[c] += 1
                    winner = side if won(nb, side) else None
                    self.visit(item.get("node"), nb, nh, side ^ 3, winner, depth + 1)
        finally:
            self.active.remove(node_id)
        self.verified_ids.add(node_id)


def verify_certificate_data(cert, max_nodes=250_000):
    if not isinstance(cert, dict) or cert.get("ruleset") != "gravity-connect5-9x7-v1":
        return {"ok": False, "error": "unsupported or malformed ruleset"}
    attacker = cert.get("attacker")
    if attacker not in ("X", "O"):
        return {"ok": False, "error": "attacker must be X or O"}
    history = cert.get("history", [])
    if not isinstance(history, list):
        return {"ok": False, "error": "history must be a list"}
    try:
        b, h, side, winner = replay(history)
        if cert.get("format") == "gravity-strategy-dag-v1":
            raw_nodes = cert.get("nodes")
            if not isinstance(raw_nodes, list) or not raw_nodes:
                raise ValueError("DAG nodes must be a non-empty list")
            nodes = {}
            for item in raw_nodes:
                if not isinstance(item, dict):
                    raise ValueError("DAG node must be an object")
                node_id = item.get("id")
                if isinstance(node_id, bool) or not isinstance(node_id, int) or node_id in nodes:
                    raise ValueError("DAG node IDs must be unique integers")
                nodes[node_id] = item
            checker = DAGChecker(attacker, nodes, max_nodes)
            checker.visit(cert.get("root"), b, h, side, winner, 0)
            if checker.verified_ids != set(nodes):
                raise ValueError("DAG contains unreachable node records")
            return {"ok": True, "nodes": checker.visits, "uniqueNodes": len(nodes),
                    "maxDepth": checker.max_depth, "format": "gravity-strategy-dag-v1"}
        checker = ExpandedChecker(attacker, max_nodes)
        checker.visit(cert.get("tree"), b, h, side, winner, 0)
        return {"ok": True, "nodes": checker.nodes, "uniqueNodes": checker.nodes,
                "maxDepth": checker.max_depth, "format": "expanded-tree"}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("certificate", type=Path)
    ap.add_argument("--max-nodes", type=int, default=250000)
    args = ap.parse_args()
    try:
        cert = json.loads(args.certificate.read_text(encoding="utf-8"))
    except Exception as exc:
        print("FAIL: invalid JSON:", exc)
        return 1
    result = verify_certificate_data(cert, args.max_nodes)
    if not result["ok"]:
        print("FAIL:", result["error"])
        return 1
    print(f"PASS certificate verified: format={result['format']} nodes={result['nodes']} uniqueNodes={result['uniqueNodes']} max_depth={result['maxDepth']}")
    print("PASS every attacker OR node has a legal winning continuation; every defender AND node covers all legal replies")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
