#!/usr/bin/env python3
"""GPC Phase 1: exact gravity-tempo reachability kernel.

This is a proof-preserving *necessary-condition* filter for one candidate
winning line. It deliberately ignores game termination during supporting
moves; that relaxation can only make a line appear more reachable, never
incorrectly eliminate a line that can really be made.

Coordinates: columns increase left-to-right; rows increase bottom-to-top.
Players are encoded as 0 and 1. A position is (p0_mask, p1_mask, heights).
"""
from __future__ import annotations

from collections import Counter
from functools import lru_cache
from typing import Iterable


def winning_lines(width: int, height: int, k: int) -> tuple[tuple[tuple[int, int], ...], ...]:
    """Enumerate every horizontal, vertical and diagonal k-cell segment once."""
    if not (1 <= k <= width and k <= height):
        raise ValueError("k must fit both board dimensions")
    lines: list[tuple[tuple[int, int], ...]] = []
    for row in range(height):
        for col in range(width - k + 1):
            lines.append(tuple((col + i, row) for i in range(k)))
    for col in range(width):
        for row in range(height - k + 1):
            lines.append(tuple((col, row + i) for i in range(k)))
    for col in range(width - k + 1):
        for row in range(height - k + 1):
            lines.append(tuple((col + i, row + i) for i in range(k)))
            lines.append(tuple((col + i, row + k - 1 - i) for i in range(k)))
    return tuple(lines)


def line_reflection_orbits(lines: Iterable[tuple[tuple[int, int], ...]], width: int):
    """Return (number of orbits, number of fixed lines) under left-right mirror."""
    normalized = {tuple(sorted(line)) for line in lines}
    reflected = {
        tuple(sorted((width - 1 - col, row) for col, row in line))
        for line in normalized
    }
    # Count orbits directly; this avoids relying on an ordering correspondence.
    unseen = set(normalized)
    orbits = 0
    while unseen:
        line = unseen.pop()
        mirror = tuple(sorted((width - 1 - col, row) for col, row in line))
        unseen.discard(mirror)
        orbits += 1
    fixed = sum(
        tuple(sorted((width - 1 - col, row) for col, row in line)) == line
        for line in normalized
    )
    return orbits, fixed


def reflect_mask(mask: int, width: int, height: int) -> int:
    out = 0
    for col in range(width):
        for row in range(height):
            if (mask >> (col * height + row)) & 1:
                out |= 1 << ((width - 1 - col) * height + row)
    return out


def canonical_state(p0: int, p1: int, heights: tuple[int, ...], height: int):
    width = len(heights)
    direct = (p0, p1, heights)
    mirrored = (
        reflect_mask(p0, width, height),
        reflect_mask(p1, width, height),
        heights[::-1],
    )
    return min(direct, mirrored)


def gravity_tempo_reachable(
    p0: int,
    p1: int,
    heights: tuple[int, ...],
    side_to_move: int,
    target_player: int,
    line: tuple[tuple[int, int], ...],
    width: int,
    height: int,
) -> bool:
    """Is there any cooperative continuation that can fill `line` for player?

    This is exact for the relaxed reachability question. Supporting cells and
    moves outside the target columns may belong to either player. Cells in the
    target line must belong to `target_player`. All column stack orders and
    global alternating turns are enforced.
    """
    target_set = set(line)
    target_cols = sorted({col for col, _ in line})
    chains: list[tuple[int, ...]] = []  # 1 = target-player required; 0 = wildcard
    goals: list[int] = []

    # An already occupied target cell belonging to the opponent can never be
    # repaired, because pieces cannot be removed.
    for col, row in line:
        if row < heights[col]:
            bit = 1 << (col * height + row)
            owner = 0 if p0 & bit else 1 if p1 & bit else None
            if owner != target_player:
                return False

    # Keep each target column's chain all the way to its top. The suffix above
    # the line's highest cell is optional wildcard capacity: it may be needed
    # as a tempo move while another target column is being completed.
    for col in target_cols:
        chain: list[int] = []
        required_indices: list[int] = []
        for row in range(heights[col], height):
            required = (col, row) in target_set
            chain.append(1 if required else 0)
            if required:
                required_indices.append(row - heights[col])
        chains.append(tuple(chain))
        goals.append(max(required_indices) if required_indices else -1)

    # All non-target columns are interchangeable wildcard moves. Their only
    # relevant property is the total number of remaining cells.
    filler_capacity = sum(
        height - heights[col] for col in range(width) if col not in target_cols
    )
    start_side = 0 if side_to_move == target_player else 1

    @lru_cache(maxsize=None)
    def search(progress: tuple[int, ...], filler_used: int, side: int) -> bool:
        # Each target cell has been passed in its column's drop sequence.
        if all(progress[i] > goals[i] for i in range(len(target_cols))):
            return True

        # Advance one target-column stack by one cell. A required line cell can
        # only be dropped by the target player; a support/suffix cell is free.
        for i, chain in enumerate(chains):
            if progress[i] >= len(chain):
                continue
            required_player = chain[progress[i]]
            if required_player == 0 or side == 0:
                nxt = list(progress)
                nxt[i] += 1
                if search(tuple(nxt), filler_used, 1 - side):
                    return True

        # Or use a move in any non-target column to change turn parity.
        if filler_used < filler_capacity:
            if search(progress, filler_used + 1, 1 - side):
                return True
        return False

    return search((0,) * len(chains), 0, start_side)


def brute_line_reachable_factory(
    width: int, height: int, line: tuple[tuple[int, int], ...], target_player: int
):
    """Independent exhaustive oracle for the relaxed line-reachability query."""
    target_mask = sum(1 << (col * height + row) for col, row in line)

    @lru_cache(maxsize=None)
    def brute(p0: int, p1: int, hs: tuple[int, ...], side: int) -> bool:
        own = p0 if target_player == 0 else p1
        if own & target_mask == target_mask:
            return True
        for col, h_now in enumerate(hs):
            if h_now >= height:
                continue
            bit = 1 << (col * height + h_now)
            if side == 0:
                q0, q1 = p0 | bit, p1
            else:
                q0, q1 = p0, p1 | bit
            nxt_hs = list(hs)
            nxt_hs[col] += 1
            if brute(q0, q1, tuple(nxt_hs), 1 - side):
                return True
        return False

    return brute


def enumerate_relaxed_states(width: int, height: int):
    """All gravity-reachable states, allowing play to continue after a win."""
    layer = {(0, 0, (0,) * width)}
    all_states = set(layer)
    layers = [layer]
    for ply in range(width * height):
        side = ply % 2
        nxt = set()
        for p0, p1, hs in layer:
            for col, h_now in enumerate(hs):
                if h_now >= height:
                    continue
                bit = 1 << (col * height + h_now)
                q0, q1 = (p0 | bit, p1) if side == 0 else (p0, p1 | bit)
                qh = list(hs)
                qh[col] += 1
                nxt.add((q0, q1, tuple(qh)))
        layer = nxt
        layers.append(layer)
        all_states.update(layer)
    return layers, all_states


def play_sequence(sequence: tuple[int, ...], width: int, height: int):
    p0 = p1 = 0
    hs = [0] * width
    for ply, col in enumerate(sequence):
        if not 0 <= col < width or hs[col] >= height:
            raise ValueError(f"illegal move at ply {ply}: column {col}")
        bit = 1 << (col * height + hs[col])
        if ply % 2 == 0:
            p0 |= bit
        else:
            p1 |= bit
        hs[col] += 1
    return p0, p1, tuple(hs), len(sequence) % 2


def has_line(mask: int, lines: Iterable[tuple[tuple[int, int], ...]], height: int) -> bool:
    return any(
        all((mask >> (col * height + row)) & 1 for col, row in line)
        for line in lines
    )


def self_test() -> dict:
    # Geometry of the target 9x7 Connect-5 board.
    target_lines = winning_lines(9, 7, 5)
    assert len(target_lines) == 92
    assert Counter(len(line) for line in target_lines) == {5: 92}
    orbits, fixed = line_reflection_orbits(target_lines, 9)
    assert (orbits, fixed) == (51, 10)

    # Exhaustive independent cross-check on every state of 4x3 Connect-3,
    # including post-win states in the relaxed transition system.
    small_lines = winning_lines(4, 3, 3)
    layers, states = enumerate_relaxed_states(4, 3)
    assert len(states) == 12_304
    checks = 0
    for line in small_lines:
        for target_player in (0, 1):
            brute = brute_line_reachable_factory(4, 3, line, target_player)
            for p0, p1, hs in states:
                side = sum(hs) % 2
                exact = gravity_tempo_reachable(
                    p0, p1, hs, side, target_player, line, 4, 3
                )
                oracle = brute(p0, p1, hs, side)
                assert exact == oracle, (line, target_player, (p0, p1, hs), exact, oracle)
                checks += 1
    assert checks == 344_512

    # A legal 40-ply 9x7 position illustrating a tempo/gravity exclusion.
    sequence = (
        5, 7, 7, 3, 5, 3, 5, 8, 7, 5, 5, 6, 5, 5, 1, 6, 7, 6, 8, 0,
        8, 8, 6, 4, 8, 8, 2, 2, 8, 7, 6, 4, 6, 0, 6, 2, 3, 7, 7, 3,
    )
    # Verify every prefix, not only the final board: the 40-ply sequence is a
    # genuinely legal nonterminal history under ordinary stop-on-win rules.
    p0 = p1 = 0
    hs_list = [0] * 9
    for ply, col in enumerate(sequence):
        bit = 1 << (col * 7 + hs_list[col])
        if ply % 2 == 0:
            p0 |= bit
        else:
            p1 |= bit
        hs_list[col] += 1
        assert not has_line(p0, target_lines, 7), f"player 0 already won at ply {ply + 1}"
        assert not has_line(p1, target_lines, 7), f"player 1 already won at ply {ply + 1}"
    hs = tuple(hs_list)
    side = len(sequence) % 2
    top_line = tuple((col, 6) for col in range(5))
    top_mask = sum(1 << (col * 7 + 6) for col in range(5))
    assert (p0 & top_mask) == 0 and (p1 & top_mask) == 0
    assert hs == (2, 1, 3, 4, 2, 7, 7, 7, 7)
    assert side == 0  # player 0 to move; target player 1 would need the line
    assert not gravity_tempo_reachable(p0, p1, hs, side, 1, top_line, 9, 7)

    # The line needs 5+6+4+3+5=23 drops in columns 0..4. Columns 5..8 are
    # full, so no filler move exists. Drop 23 is player 0's turn, not player 1's.
    needed_drops = sum(7 - hs[col] for col in range(5))
    assert needed_drops == 23 and sum(7 - hs[col] for col in range(5, 9)) == 0
    assert needed_drops % 2 == 1

    # Early target-board state counts; no win is possible before ply 9.
    layer = {(0, 0, (0,) * 9)}
    raw_counts = []
    mirror_counts = []
    for ply in range(9):
        raw_counts.append(len(layer))
        mirror_counts.append(len({canonical_state(a, b, hs, 7) for a, b, hs in layer}))
        nxt = set()
        side_now = ply % 2
        for a, b, hs in layer:
            for col, h_now in enumerate(hs):
                if h_now >= 7:
                    continue
                bit = 1 << (col * 7 + h_now)
                q0, q1 = (a | bit, b) if side_now == 0 else (a, b | bit)
                qh = list(hs)
                qh[col] += 1
                nxt.add((q0, q1, tuple(qh)))
        layer = nxt
    expected_raw = [1, 9, 81, 477, 2745, 12285, 55989, 214695, 844704]
    expected_mirror = [1, 5, 41, 241, 1385, 6161, 28025, 107423, 422572]
    assert raw_counts == expected_raw
    assert mirror_counts == expected_mirror

    return {
        "target_board": "9x7 Connect-5 with gravity",
        "winning_segments": len(target_lines),
        "mirror_orbits": orbits,
        "mirror_fixed_segments": fixed,
        "small_board_relaxed_states": len(states),
        "independent_line_reachability_checks": checks,
        "tempo_gravity_counterexample_position_ply": len(sequence),
        "tempo_gravity_example_required_drops": needed_drops,
        "raw_state_counts_plies_0_to_8": raw_counts,
        "mirror_state_counts_plies_0_to_8": mirror_counts,
    }


if __name__ == "__main__":
    import json
    print(json.dumps(self_test(), indent=2))
