#!/usr/bin/env python3
"""GPC Phase 2: proof-producing AND/OR engine + verified tactical macros.

Target family: gravity Connect-K on a W x H board.
State: (p0_mask, p1_mask, heights_tuple, side_to_move).
Bits are column-major: bit_index = column * height + row, rows bottom-up.

No heuristic evaluation is used for proof decisions. Tactical macros are exact
lemmas with executable precondition checkers. The recursive solver returns
W/D/L from the point of view of the side to move. Certificates are verified
by a separate code path that does not consult the solver/transposition table.
"""
from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from collections import Counter, deque
from typing import Iterable, Optional
import json
import argparse
import hashlib
import time


@dataclass(frozen=True)
class Game:
    width: int
    height: int
    k: int
    line_masks: tuple[int, ...]
    cell_lines: tuple[tuple[int, ...], ...]

    @staticmethod
    def make(width: int, height: int, k: int) -> "Game":
        if width < k or height < k or k < 2:
            raise ValueError("require width,height >= k >= 2")
        lines = []
        for r in range(height):
            for c in range(width-k+1):
                lines.append(sum(1 << ((c+i)*height+r) for i in range(k)))
        for c in range(width):
            for r in range(height-k+1):
                lines.append(sum(1 << (c*height+r+i) for i in range(k)))
        for c in range(width-k+1):
            for r in range(height-k+1):
                lines.append(sum(1 << ((c+i)*height+(r+i)) for i in range(k)))
                lines.append(sum(1 << ((c+i)*height+(r+k-1-i)) for i in range(k)))
        by_cell=[[] for _ in range(width*height)]
        for line in lines:
            for idx in range(width*height):
                if (line >> idx) & 1:
                    by_cell[idx].append(line)
        return Game(width, height, k, tuple(lines), tuple(tuple(x) for x in by_cell))

    @property
    def empty_heights(self) -> tuple[int, ...]:
        return (0,) * self.width

    def has_win(self, mask: int) -> bool:
        return any((mask & line) == line for line in self.line_masks)

    def mirror_mask(self, mask: int) -> int:
        out = 0
        for c in range(self.width):
            col_mask = ((1 << self.height) - 1) << (c*self.height)
            chunk = (mask & col_mask) >> (c*self.height)
            out |= chunk << ((self.width-1-c)*self.height)
        return out

    def canonical(self, s: tuple[int, int, tuple[int, ...], int]):
        p0, p1, hs, side = s
        a = (p0, p1, hs, side)
        b = (self.mirror_mask(p0), self.mirror_mask(p1), hs[::-1], side)
        return min(a, b)

    def winner(self, s) -> Optional[int]:
        p0, p1, _, _ = s
        w0, w1 = self.has_win(p0), self.has_win(p1)
        if w0 and w1:
            raise ValueError("invalid state: both players have winning lines")
        if w0: return 0
        if w1: return 1
        return None

    def terminal(self, s) -> bool:
        return self.winner(s) is not None or all(h == self.height for h in s[2])

    def legal_moves(self, s) -> tuple[int, ...]:
        if self.terminal(s):
            return ()
        return tuple(c for c,h in enumerate(s[2]) if h < self.height)

    def play(self, s, col: int):
        p0, p1, hs, side = s
        if not 0 <= col < self.width or hs[col] >= self.height:
            raise ValueError("illegal column")
        bit = 1 << (col*self.height + hs[col])
        nh = list(hs); nh[col] += 1
        if side == 0:
            return (p0 | bit, p1, tuple(nh), 1)
        return (p0, p1 | bit, tuple(nh), 0)

    def legal_history(self, moves: Iterable[int]):
        s = (0, 0, self.empty_heights, 0)
        for i,c in enumerate(moves):
            if self.terminal(s):
                raise ValueError(f"history continues after terminal at ply {i}")
            s = self.play(s, c)
        return s

    def pretty(self, s) -> str:
        p0,p1,hs,side=s
        rows=[]
        for r in reversed(range(self.height)):
            row=[]
            for c in range(self.width):
                bit=1 << (c*self.height+r)
                row.append('X' if p0&bit else 'O' if p1&bit else '.')
            rows.append(' '.join(row))
        rows.append(' '.join(str(i) for i in range(self.width)))
        return '\n'.join(rows)


WIN, DRAW, LOSS = 1, 0, -1


def immediate_winning_columns(game: Game, s, player: int) -> tuple[int, ...]:
    """Columns whose next drop by `player` completes a line immediately."""
    p0,p1,hs,side=s
    if game.terminal(s): return ()
    out=[]
    for c,h in enumerate(hs):
        if h < game.height and immediate_winning_in_column(game,s,player,c):
            out.append(c)
    return tuple(out)


def immediate_winning_in_column(game: Game, s, player: int, col: int) -> bool:
    """Whether player's next drop specifically in `col` wins immediately."""
    if game.terminal(s): return False
    p0,p1,hs,side=s
    if not 0 <= col < game.width or hs[col] >= game.height: return False
    own = p0 if player == 0 else p1
    idx=col*game.height+hs[col]
    bit = 1 << idx
    after=own|bit
    return any((after & line)==line for line in game.cell_lines[idx])


def naive_safe_moves(game: Game, s) -> tuple[int, ...]:
    """Slow independent definition: test every move and all opponent win columns."""
    if game.terminal(s): return ()
    side=s[3]; safe=[]
    for c in game.legal_moves(s):
        child=game.play(s,c)
        mover_mask=child[0] if side==0 else child[1]
        if game.has_win(mover_mask):
            safe.append(c)
        elif not immediate_winning_columns(game,child,1-side):
            safe.append(c)
    return tuple(safe)


def column_theorem_safe_moves(game: Game, s) -> tuple[int, ...]:
    """Fast exact reply set from the One-Column Defence Theorem.

    A drop changes only the landing cell and height of its own column. Thus an
    opponent's already-playable winning cell in any *other* column survives.
    A move can neutralize at most the old threat in its chosen column; but the
    new landing cell one row higher in that same column might itself be an
    opponent immediate win. Immediate wins by the mover override defence.
    """
    if game.terminal(s): return ()
    side=s[3]
    legal=game.legal_moves(s)
    own_wins=set(immediate_winning_columns(game,s,side))
    old_enemy_wins=set(immediate_winning_columns(game,s,1-side))
    safe=[]
    for c in legal:
        if c in own_wins:
            safe.append(c)
            continue
        # Any old enemy threat in another column remains untouched.
        if any(w != c for w in old_enemy_wins):
            continue
        child=game.play(s,c)
        # Only this column has changed. The opponent's next landing square in
        # it is a newly exposed stacked threat if it completes their line.
        if immediate_winning_in_column(game,child,1-side,c):
            continue
        safe.append(c)
    return tuple(safe)


def tactical_profile(game: Game, s) -> dict:
    """Exact tactical summary using the column-local theorem."""
    if game.terminal(s):
        return {"terminal": True, "winner": game.winner(s), "safe_moves": [],
                "unsafe_moves": [], "fork_moves": [], "immediate_wins": [],
                "old_opponent_winning_columns": []}
    side=s[3]; legal=game.legal_moves(s)
    wins=immediate_winning_columns(game,s,side)
    old_enemy=immediate_winning_columns(game,s,1-side)
    safe=list(column_theorem_safe_moves(game,s))
    unsafe=[c for c in legal if c not in set(safe)]
    forks=[]
    for c in safe:
        child=game.play(s,c)
        mover_mask=child[0] if side==0 else child[1]
        if game.has_win(mover_mask): continue
        own_next=immediate_winning_columns(game,child,side)
        if len(own_next)>=2:
            forks.append({"move":c,"winning_columns":list(own_next)})
    # This map is diagnostic, not used by the solver. It is intentionally
    # calculated with the direct successor checker for transparency.
    opp_after={}
    for c in legal:
        child=game.play(s,c)
        mover_mask=child[0] if side==0 else child[1]
        opp_after[str(c)]=[] if game.has_win(mover_mask) else list(immediate_winning_columns(game,child,1-side))
    return {"terminal":False,"immediate_wins":list(wins),
            "old_opponent_winning_columns":list(old_enemy),
            "safe_moves":safe,"unsafe_moves":unsafe,
            "fork_moves":forks,"opponent_wins_after":opp_after}


def search_tactical_reduction(game: Game, s) -> tuple[tuple[int, ...], Optional[dict]]:
    """Low-overhead tactical reduction used inside exact search.

    Assumes the caller has already checked that side-to-move has no immediate
    winning move. Returns the exact moves that avoid an immediate opponent win,
    plus the first verified two-column fork if one exists.
    """
    side=s[3]; opp=1-side
    legal=game.legal_moves(s)
    old_enemy=set(immediate_winning_columns(game,s,opp))
    if len(old_enemy)>=2:
        return (),None
    if len(old_enemy)==1:
        candidates=(next(iter(old_enemy)),)
    else:
        candidates=legal
    safe=[]
    for c in candidates:
        child=game.play(s,c)
        if immediate_winning_in_column(game,child,opp,c):
            continue
        safe.append(c)
    if not safe:
        return (),None
    for c in safe:
        child=game.play(s,c)
        own_next=immediate_winning_columns(game,child,side)
        if len(own_next)>=2:
            return tuple(safe),{"move":c,"winning_columns":list(own_next)}
    return tuple(safe),None


class ExactSolver:
    """Exact W/D/L alpha-beta-free minimax with symmetry TT and tactical lemmas."""
    def __init__(self, game: Game, use_tactics: bool = True):
        self.game=game
        self.use_tactics=use_tactics
        self.nodes=0
        self.tactic_hits=Counter()
        self._solve = lru_cache(maxsize=None)(self._solve_cached)

    def value(self, s) -> int:
        return self._solve(self.game.canonical(s))

    def _solve_cached(self, s) -> int:
        self.nodes += 1
        g=self.game
        winner=g.winner(s)
        if winner is not None:
            return LOSS if s[3] != winner else WIN
        moves=g.legal_moves(s)
        if not moves: return DRAW

        if self.use_tactics:
            side=s[3]
            wins=immediate_winning_columns(g,s,side)
            if wins:
                self.tactic_hits["immediate_win"] += 1
                return WIN
            safe_moves,fork=search_tactical_reduction(g,s)
            # Every move allows the opponent an immediate win: exact forced loss.
            if not safe_moves:
                self.tactic_hits["no_safe_reply"] += 1
                return LOSS
            # A safe move that creates two distinct immediate winning columns
            # is a forced win in at most two plies.
            if fork is not None:
                self.tactic_hits["double_threat_fork"] += 1
                return WIN
            moves=safe_moves

        saw_draw=False
        for c in moves:
            child=g.play(s,c)
            v=-self._solve(g.canonical(child))
            if v == WIN:
                return WIN
            if v == DRAW:
                saw_draw=True
        return DRAW if saw_draw else LOSS

    def clear(self):
        self._solve.cache_clear(); self.nodes=0; self.tactic_hits.clear()

    def cache_info(self):
        return self._solve.cache_info()


def is_target_win(game: Game, s, target: int, value: int) -> bool:
    return value == (WIN if s[3] == target else LOSS)


def find_compound_fork_move(game: Game, s, target: int) -> Optional[dict]:
    """Find a legal move that leaves no opponent immediate win but >=2 target wins."""
    if game.terminal(s) or s[3] != target: return None
    if immediate_winning_columns(game,s,target): return None
    for c in game.legal_moves(s):
        child=game.play(s,c)
        if game.winner(child) is not None:  # an immediate win is a separate macro
            continue
        if immediate_winning_columns(game,child,1-target):
            continue
        wins=immediate_winning_columns(game,child,target)
        if len(wins)>=2:
            return {"move":c,"winning_columns":list(wins)}
    return None


def find_same_column_ladder(game: Game, s, target: int) -> Optional[dict]:
    """Find the unique forced block which still exposes a stacked win."""
    if game.terminal(s) or s[3] != target: return None
    if immediate_winning_columns(game,s,target): return None
    enemy=immediate_winning_columns(game,s,1-target)
    if len(enemy)!=1: return None
    c=enemy[0]
    child=game.play(s,c)
    if immediate_winning_in_column(game,child,1-target,c):
        return {"blocking_column":c,"next_winning_column":c}
    return None


def build_certificate(game: Game, solver: ExactSolver, root, target: int,
                      claim: str = "target_win", root_history: Iterable[int] = ()) -> dict:
    """Build an AND/OR proof DAG for target_win or target_not_win."""
    if claim not in ("target_win", "target_not_win"):
        raise ValueError("unsupported claim")
    nodes=[]; index={}

    def rec(s):
        key=(game.canonical(s),target,claim)
        if key in index: return index[key]
        nid=len(nodes); index[key]=nid
        # Reserve before recursion; game is acyclic, so no back-edge can arise.
        nodes.append(None)
        winner=game.winner(s)
        if winner is not None or not game.legal_moves(s):
            ok = (winner == target) if claim == "target_win" else (winner != target)
            if not ok: raise AssertionError("requested claim is false at leaf")
            nodes[nid]={"id":nid,"kind":"terminal","state":encode_state(s),
                        "winner":winner,"claim":claim}
            return nid
        value=solver.value(s)
        target_wins=is_target_win(game,s,target,value)
        if claim == "target_win" and not target_wins:
            raise AssertionError("target_win claim does not hold")
        if claim == "target_not_win" and target_wins:
            raise AssertionError("target_not_win claim does not hold")

        prof=tactical_profile(game,s)
        if claim == "target_win" and s[3] == target:
            wins=immediate_winning_columns(game,s,target)
            if wins:
                nodes[nid]={"id":nid,"kind":"macro_immediate_win",
                            "state":encode_state(s),"target":target,
                            "winning_columns":list(wins),"claim":claim}
                return nid
            fork=find_compound_fork_move(game,s,target)
            if fork is not None:
                nodes[nid]={"id":nid,"kind":"macro_compound_fork",
                            "state":encode_state(s),"target":target,
                            "move":fork["move"],
                            "winning_columns":fork["winning_columns"],"claim":claim}
                return nid
        if claim == "target_win" and s[3] != target:
            own_threats=immediate_winning_columns(game,s,target)
            opponent_wins=immediate_winning_columns(game,s,s[3])
            if not opponent_wins and len(own_threats)>=2:
                nodes[nid]={"id":nid,"kind":"macro_double_threat",
                            "state":encode_state(s),"target":target,
                            "winning_columns":list(own_threats),"claim":claim}
                return nid
        if claim == "target_not_win" and s[3] == target:
            own_now=immediate_winning_columns(game,s,target)
            enemy_now=immediate_winning_columns(game,s,1-target)
            if not own_now and len(enemy_now)>=2:
                nodes[nid]={"id":nid,"kind":"macro_double_opponent_threats",
                            "state":encode_state(s),"target":target,
                            "winning_columns":list(enemy_now),"claim":claim}
                return nid
            ladder=find_same_column_ladder(game,s,target)
            if ladder is not None:
                nodes[nid]={"id":nid,"kind":"macro_same_column_ladder",
                            "state":encode_state(s),"target":target,
                            **ladder,"claim":claim}
                return nid
            if not own_now and not prof["safe_moves"]:
                nodes[nid]={"id":nid,"kind":"macro_no_safe_reply",
                            "state":encode_state(s),"target":target,
                            "claim":claim}
                return nid

        side=s[3]; moves=game.legal_moves(s)
        if claim == "target_win":
            # OR when target moves; AND when the opponent moves.
            if side == target:
                c=None
                for candidate in moves:
                    child_candidate=game.play(s,candidate)
                    if is_target_win(game,child_candidate,target,solver.value(child_candidate)):
                        c=candidate; break
                if c is None: raise AssertionError("missing OR witness")
                child=game.play(s,c)
                nodes[nid]={"id":nid,"kind":"OR","state":encode_state(s),
                            "target":target,"move":c,"child":rec(child),"claim":claim}
            else:
                children=[]
                for c in moves:
                    child=game.play(s,c)
                    if not is_target_win(game,child,target,solver.value(child)):
                        raise AssertionError("AND branch is not a target win")
                    children.append({"move":c,"child":rec(child)})
                nodes[nid]={"id":nid,"kind":"AND","state":encode_state(s),
                            "target":target,"children":children,"claim":claim}
        else:
            # To refute target's forced win: all target moves fail (OR node),
            # while one opponent reply suffices (AND node).
            if side == target:
                children=[]
                for c in moves:
                    child=game.play(s,c)
                    if is_target_win(game,child,target,solver.value(child)):
                        raise AssertionError("not-win OR node has a winning child")
                    children.append({"move":c,"child":rec(child)})
                nodes[nid]={"id":nid,"kind":"AND_NOT_WIN","state":encode_state(s),
                            "target":target,"children":children,"claim":claim}
            else:
                choices=[c for c in moves if not is_target_win(game,game.play(s,c),
                          target,solver.value(game.play(s,c)))]
                if not choices: raise AssertionError("missing opponent counter-witness")
                c=choices[0]
                nodes[nid]={"id":nid,"kind":"OR_NOT_WIN","state":encode_state(s),
                            "target":target,"move":c,"child":rec(game.play(s,c)),
                            "claim":claim}
        return nid

    root_id=rec(root)
    cert={"format":"GPC-ANDOR-CERT-1","game":{"width":game.width,
          "height":game.height,"k":game.k},"root":root_id,"target":target,
          "claim":claim,"root_history":list(root_history),"nodes":nodes}
    cert["sha256"]=hashlib.sha256(json.dumps(cert,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    return cert


def encode_state(s):
    p0,p1,hs,side=s
    return {"p0":hex(p0),"p1":hex(p1),"heights":list(hs),"side":side}


def decode_state(d):
    return (int(d["p0"],16),int(d["p1"],16),tuple(d["heights"]),int(d["side"]))


def verify_certificate(cert: dict) -> tuple[bool,str]:
    """Independent structural checker; does not invoke ExactSolver."""
    if cert.get("format") != "GPC-ANDOR-CERT-1": return False,"unknown format"
    gdata=cert["game"]
    game=Game.make(gdata["width"],gdata["height"],gdata["k"])
    nodes=cert["nodes"]
    claimed_hash=cert.get("sha256")
    payload={k:v for k,v in cert.items() if k!="sha256"}
    actual_hash=hashlib.sha256(json.dumps(payload,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    if claimed_hash != actual_hash: return False,"certificate hash mismatch"
    byid={n["id"]:n for n in nodes}
    if len(byid)!=len(nodes): return False,"duplicate node IDs"
    claim=cert["claim"]; target=cert["target"]
    if claim not in ("target_win","target_not_win") or target not in (0,1):
        return False,"bad claim/target"
    # Reconstruct the root from the empty board so the certificate cannot
    # smuggle in a parity-correct but unreachable stack assignment.
    try:
        root_state=(0,0,game.empty_heights,0)
        for ply,col in enumerate(cert.get("root_history", [])):
            if game.terminal(root_state): return False,"root history continues after terminal"
            if col not in game.legal_moves(root_state): return False,"illegal root-history move"
            root_state=game.play(root_state,col)
        root_node=byid.get(cert.get("root"))
        if root_node is None: return False,"missing root node"
        if game.canonical(decode_state(root_node["state"])) != game.canonical(root_state):
            return False,"root history does not reconstruct certificate root"
    except Exception as e:
        return False,"invalid root history: "+repr(e)
    seen=set()

    def check(nid):
        if nid in seen: return True,""
        if nid not in byid: return False,"missing child node"
        seen.add(nid); n=byid[nid]; s=decode_state(n["state"])
        try:
            # Validate occupancy/gravity/turn count and no double winners.
            p0,p1,hs,side=s
            if len(hs)!=game.width or any(h<0 or h>game.height for h in hs):
                return False,"invalid heights"
            occ=sum(1 << (c*game.height+r) for c,h in enumerate(hs) for r in range(h))
            if (p0|p1)!=occ or (p0&p1): return False,"occupancy mismatch"
            if sum(hs)%2 != (0 if side==0 else 1): return False,"turn parity mismatch"
            n0,n1=p0.bit_count(),p1.bit_count()
            if not (n0==n1 if side==0 else n0==n1+1): return False,"piece-count parity mismatch"
            winner=game.winner(s)
            if n["kind"]=="terminal":
                if not (winner is not None or all(h==game.height for h in hs)):
                    return False,"nonterminal leaf"
                ok=(winner==target) if claim=="target_win" else (winner!=target)
                if not ok: return False,"terminal leaf contradicts claim"
                return True,""
            if winner is not None or all(h==game.height for h in hs):
                return False,"expanded terminal state"
            moves=game.legal_moves(s)
            kind=n["kind"]
            if kind=="macro_immediate_win":
                if claim!="target_win" or side!=target: return False,"bad immediate-win macro context"
                wins=immediate_winning_columns(game,s,target)
                if not wins or n.get("winning_columns")!=list(wins): return False,"immediate-win witness mismatch"
                return True,""
            if kind=="macro_compound_fork":
                if claim!="target_win" or side!=target: return False,"bad compound-fork context"
                if immediate_winning_columns(game,s,target): return False,"immediate win should be used first"
                c=n["move"]
                if c not in moves: return False,"illegal fork-setup move"
                child=game.play(s,c)
                if game.winner(child) is not None: return False,"terminal move is not a fork macro"
                if immediate_winning_columns(game,child,1-target): return False,"opponent has immediate win after setup"
                wins=immediate_winning_columns(game,child,target)
                if len(wins)<2 or n.get("winning_columns")!=list(wins): return False,"fork witness mismatch"
                return True,""
            if kind=="macro_double_threat":
                if claim!="target_win" or side==target: return False,"bad fork macro context"
                if immediate_winning_columns(game,s,side): return False,"opponent has immediate win"
                wins=immediate_winning_columns(game,s,target)
                if len(wins)<2 or n.get("winning_columns")!=list(wins): return False,"fewer than two distinct winning columns"
                return True,""
            if kind=="macro_double_opponent_threats":
                if claim!="target_not_win" or side!=target: return False,"bad opponent-double-threat context"
                if immediate_winning_columns(game,s,target): return False,"target has immediate win"
                wins=immediate_winning_columns(game,s,1-target)
                if len(wins)<2 or n.get("winning_columns")!=list(wins): return False,"opponent threat columns mismatch"
                return True,""
            if kind=="macro_same_column_ladder":
                if claim!="target_not_win" or side!=target: return False,"bad ladder context"
                if immediate_winning_columns(game,s,target): return False,"target has immediate win"
                enemy=immediate_winning_columns(game,s,1-target)
                if len(enemy)!=1 or enemy[0]!=n.get("blocking_column"): return False,"not a unique forced block"
                c=enemy[0]
                child=game.play(s,c)
                if not immediate_winning_in_column(game,child,1-target,c): return False,"blocking move does not expose stacked win"
                if n.get("next_winning_column")!=c: return False,"wrong ladder witness"
                return True,""
            if kind=="macro_no_safe_reply":
                if claim!="target_not_win" or side!=target: return False,"bad no-safe-reply context"
                if immediate_winning_columns(game,s,target): return False,"target has immediate win"
                for c in moves:
                    child=game.play(s,c)
                    mover=target
                    if game.has_win(child[0] if mover==0 else child[1]):
                        return False,"target has immediate winning move"
                    if not immediate_winning_columns(game,child,1-target):
                        return False,"found a safe reply; macro invalid"
                return True,""
            if kind in ("OR","AND","AND_NOT_WIN","OR_NOT_WIN"):
                if n.get("claim")!=claim or n.get("target")!=target:
                    return False,"node claim mismatch"
                if kind=="OR":
                    if claim!="target_win" or side!=target: return False,"OR role mismatch"
                    c=n["move"]
                    if c not in moves: return False,"illegal OR move"
                    child=game.play(s,c)
                    cn=byid.get(n["child"])
                    if cn is None or game.canonical(decode_state(cn["state"]))!=game.canonical(child): return False,"OR child state mismatch"
                    return check(n["child"])
                if kind=="AND":
                    if claim!="target_win" or side==target: return False,"AND role mismatch"
                    entries=n["children"]
                    if sorted(x["move"] for x in entries)!=sorted(moves): return False,"AND does not cover all legal moves"
                    for e in entries:
                        child=game.play(s,e["move"]); cn=byid.get(e["child"])
                        if cn is None or game.canonical(decode_state(cn["state"]))!=game.canonical(child): return False,"AND child state mismatch"
                        ok,msg=check(e["child"])
                        if not ok:return ok,msg
                    return True,""
                if kind=="AND_NOT_WIN":
                    if claim!="target_not_win" or side!=target: return False,"not-win AND role mismatch"
                    entries=n["children"]
                    if sorted(x["move"] for x in entries)!=sorted(moves): return False,"not-win proof misses target move"
                    for e in entries:
                        child=game.play(s,e["move"]); cn=byid.get(e["child"])
                        if cn is None or game.canonical(decode_state(cn["state"]))!=game.canonical(child):return False,"not-win child mismatch"
                        ok,msg=check(e["child"])
                        if not ok:return ok,msg
                    return True,""
                if kind=="OR_NOT_WIN":
                    if claim!="target_not_win" or side==target:return False,"not-win OR role mismatch"
                    c=n["move"]
                    if c not in moves:return False,"illegal counter-move"
                    child=game.play(s,c); cn=byid.get(n["child"])
                    if cn is None or game.canonical(decode_state(cn["state"]))!=game.canonical(child):return False,"counter child mismatch"
                    return check(n["child"])
            return False,"unknown node kind: "+str(kind)
        except Exception as e:
            return False,"exception while checking node: "+repr(e)

    ok,msg=check(cert["root"])
    if not ok:return False,msg
    # Ensure no dangling nodes were smuggled into the artifact.
    if len(seen)!=len(nodes): return False,"certificate contains unreachable nodes"
    return True,f"verified {len(seen)} nodes"


def enumerate_legal_states(game: Game, max_ply: Optional[int]=None):
    """Enumerate unique legal states; stop each path at first terminal."""
    start=(0,0,game.empty_heights,0)
    seen={game.canonical(start)}
    q=deque([start])
    while q:
        s=q.popleft(); ply=sum(s[2])
        yield s
        if max_ply is not None and ply>=max_ply: continue
        for c in game.legal_moves(s):
            ch=game.play(s,c); key=game.canonical(ch)
            if key not in seen:
                seen.add(key); q.append(ch)


def validate_tactics_exhaustively(game: Game) -> dict:
    solver=ExactSolver(game, use_tactics=False)
    counts=Counter(); states=0
    for s in enumerate_legal_states(game):
        states += 1
        if game.terminal(s): continue
        side=s[3]
        wins=immediate_winning_columns(game,s,side)
        v=solver.value(s)
        if wins:
            counts["immediate_win_states"] += 1
            assert v==WIN, ("immediate-win lemma unsound",game,s,v)
        prof=tactical_profile(game,s)
        naive=naive_safe_moves(game,s)
        fast=tuple(prof["safe_moves"])
        assert set(fast)==set(naive), ("one-column theorem mismatch",game,s,fast,naive)
        counts["column_theorem_vs_naive_states"] += 1
        if not wins and not fast:
            counts["no_safe_reply_states"] += 1
            assert v==LOSS, ("no-safe-reply lemma unsound",game,s,v)
        if prof["fork_moves"]:
            counts["double_threat_fork_states"] += 1
            assert v==WIN, ("double-threat lemma unsound",game,s,v)
        counts["states_with_forced_reply_restriction"] += bool(prof["unsafe_moves"] and prof["safe_moves"])
    counts["states_enumerated"] = states
    counts["solver_nodes"] = solver.nodes
    return dict(counts)


def run_self_tests() -> dict:
    results={}
    # Nontrivial exact AND/OR game: 4x3 connect-3.
    game=Game.make(4,3,3)
    assert len(game.line_masks)==14
    t0=time.time()
    tactical=validate_tactics_exhaustively(game)
    solver=ExactSolver(game,use_tactics=True)
    root=(0,0,game.empty_heights,0)
    root_value=solver.value(root)
    certs={}
    for target in (0,1):
        claim="target_win" if (root_value==WIN if target==0 else root_value==LOSS) else "target_not_win"
        cert=build_certificate(game,solver,root,target,claim)
        ok,msg=verify_certificate(cert)
        assert ok,(target,claim,msg)
        certs[str(target)]={"claim":claim,"nodes":len(cert["nodes"]),"verifier":msg,
                           "sha256":cert["sha256"]}
    # A second parameter regime: Connect-4 with same gravity and a different k.
    game2=Game.make(4,4,4)
    solver2=ExactSolver(game2,use_tactics=True)
    root2=(0,0,game2.empty_heights,0)
    root2_value=solver2.value(root2)
    baseline2=ExactSolver(game2,use_tactics=False)
    baseline2_value=baseline2.value(root2)
    assert baseline2_value==root2_value, "tactical solver disagrees with unpruned minimax"
    cert2s={}
    for target in (0,1):
        claim2="target_win" if (root2_value==WIN if target==0 else root2_value==LOSS) else "target_not_win"
        cert2=build_certificate(game2,solver2,root2,target,claim2)
        ok2,msg2=verify_certificate(cert2); assert ok2,msg2
        cert2s[str(target)]={"claim":claim2,"nodes":len(cert2["nodes"]),"verifier":msg2,
                            "sha256":cert2["sha256"]}
        if target==0:
            tampered=json.loads(json.dumps(cert2))
            # Corrupt the root proof node's branch/move: the checker must reject.
            rootnode=next(n for n in tampered["nodes"] if n["id"]==tampered["root"])
            if rootnode["kind"] in ("OR","OR_NOT_WIN"):
                rootnode["move"]=(rootnode["move"]+1)%game2.width
            elif rootnode["kind"] in ("AND","AND_NOT_WIN"):
                rootnode["children"].pop()
            tampered.pop("sha256",None)
            tampered["sha256"]=hashlib.sha256(json.dumps(tampered,sort_keys=True,separators=(",",":")).encode()).hexdigest()
            tamper_ok,_=verify_certificate(tampered)
            assert not tamper_ok, "structurally tampered certificate was accepted"
    results["4x3_connect3"]={"root_wdl_from_first_player_view":("P0_WIN" if root_value==WIN else "DRAW" if root_value==DRAW else "P1_WIN"),
       "exact_nodes":solver.nodes,"cache":str(solver.cache_info()),"tactic_hits":dict(solver.tactic_hits),
       "exhaustive_tactical_audit":tactical,"certificates":certs}
    results["4x4_connect4"]={"root_wdl_from_first_player_view":("P0_WIN" if root2_value==WIN else "DRAW" if root2_value==DRAW else "P1_WIN"),
       "exact_nodes_with_tactics":solver2.nodes,"exact_nodes_without_tactics":baseline2.nodes,
       "unpruned_minimax_agreement":"passed","tactic_hits":dict(solver2.tactic_hits),
       "certificates":cert2s,"tamper_rejection_test":"passed"}
    results["target_geometry"]={"board":"9x7 Connect-5","winning_segments":len(Game.make(9,7,5).line_masks)}
    results["elapsed_seconds"]=round(time.time()-t0,3)
    return results


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--self-test",action="store_true")
    ap.add_argument("--board",nargs=3,type=int,metavar=("W","H","K"))
    ap.add_argument("--history",default="",help="comma-separated zero-based columns")
    ap.add_argument("--solve",action="store_true")
    ap.add_argument("--certificate",default="")
    ap.add_argument("--verify",default="",help="independently verify a GPC-ANDOR-CERT-1 JSON file")
    args=ap.parse_args()
    if args.verify:
        with open(args.verify,"r",encoding="utf-8") as f:
            cert=json.load(f)
        ok,msg=verify_certificate(cert)
        print(json.dumps({"verified":ok,"message":msg},indent=2))
        if not ok: raise SystemExit(1)
        return
    if args.self_test or (not args.board and not args.solve):
        print(json.dumps(run_self_tests(),indent=2)); return
    if not args.board: raise SystemExit("--board W H K required")
    game=Game.make(*args.board)
    moves=tuple(int(x) for x in args.history.split(",") if x.strip()!="")
    s=game.legal_history(moves)
    prof=tactical_profile(game,s)
    out={"board":args.board,"history":moves,"state":encode_state(s),"tactical_profile":prof}
    if args.solve:
        sol=ExactSolver(game,use_tactics=True)
        v=sol.value(s)
        out.update({"wdl_from_side_to_move":{WIN:"WIN",DRAW:"DRAW",LOSS:"LOSS"}[v],
                    "nodes":sol.nodes,"tactic_hits":dict(sol.tactic_hits),"cache":str(sol.cache_info())})
        if args.certificate:
            target=0 if (v==WIN and s[3]==0) or (v==LOSS and s[3]==1) else 1
            claim="target_win" if is_target_win(game,s,target,v) else "target_not_win"
            cert=build_certificate(game,sol,s,target,claim,root_history=moves)
            ok,msg=verify_certificate(cert)
            if not ok: raise RuntimeError("generated certificate failed: "+msg)
            with open(args.certificate,"w") as f: json.dump(cert,f,separators=(",",":"))
            out["certificate"]={"path":args.certificate,"nodes":len(cert["nodes"]),"verification":msg}
    print(json.dumps(out,indent=2))

if __name__=="__main__": main()
