# GPC — Phase 1 Report
## Structural reductions and exact gravity-tempo reachability

**Target model.** Two players alternate dropping one token into one of 9 columns of height 7. A token occupies the lowest empty cell in its chosen column. A player wins on the first horizontal, vertical, or diagonal run of 5. A full board without a run is a draw. Coordinates in the computation are column-major and rows are numbered from the bottom.

This is the formal model used for this phase. It should be compared with the game's in-app rules panel before any final game-theoretic claim is made, especially if there are nonstandard rules such as a different terminal condition.

## Result A — exact geometry and symmetry

The target board has exactly:

- 35 horizontal winning segments: `7 × (9 − 5 + 1)`;
- 27 vertical winning segments: `9 × (7 − 5 + 1)`;
- 30 diagonal winning segments: `2 × (9 − 5 + 1) × (7 − 5 + 1)`;
- **92 total winning segments**.

Left-right reflection has 10 fixed segments: 7 middle horizontal segments and 3 vertical segments in the middle column. By Burnside's lemma the 92 segments form **51 reflection orbits**. The same reflection is an automorphism of the complete game: it maps legal drops to legal drops, preserves gravity and turn order, and maps winning segments to winning segments.

At the initial position, the nine possible first columns therefore reduce to **five representatives**: four mirrored pairs and the center column. This is an exact symmetry reduction, not an opening heuristic.

## Result B — Gravity-Tempo Reachability (GTR)

A candidate winning line may be geometrically empty yet impossible for a player to complete because gravity forces a sequence of prerequisite drops, while turns alternate. We formalize this as follows.

For a fixed candidate line `L` and target player `P`:

1. If any cell of `L` is already occupied by the opponent, `L` is permanently impossible for `P`.
2. For every column touched by `L`, construct its remaining bottom-to-top drop chain up to the top of the board. A cell belonging to `L` is a required task for `P`; every other cell in the chain is a wildcard support/tempo task.
3. All remaining cells in columns not touched by `L` are interchangeable wildcard tasks. Only their total capacity matters.
4. A dynamic program interleaves the column chains while enforcing the global alternating player-to-move. Required tasks may be executed only on `P`'s turns; wildcard tasks may be executed by either player.
5. The line is reachable in the relaxed model exactly when every required task can be passed in some such interleaving.

### Lemma (safe line-impossibility filter)

If GTR returns `false` for `(position, side-to-move, P, L)`, then no legal continuation of the actual game can give player `P` all cells of `L`.

**Proof.** Any actual continuation that completes `L` induces an interleaving of the remaining cell-drop chains: gravity forces each column's cells to be dropped in bottom-to-top order, and the turns alternate globally. Every cell of `L` that was empty must be dropped by `P`; all other drops are wildcard tasks. The GTR dynamic program enumerates every interleaving satisfying precisely these necessary constraints. It is deliberately more permissive than the actual game because it allows play to continue through any other winning line that might be formed on the way. Thus every actual line-completing continuation is included in the relaxed schedule set. If the relaxed DP finds no schedule, an actual continuation cannot exist. ∎

**Interpretation.** `true` means only “not ruled out”: it does not prove a forced win, or even that the opponent will cooperate. `false` is a sound impossibility result for that one candidate line. The filter is useful for removing dead line targets before tactical proof search.

## Result C — adversarial validation

The implementation was compared against a separately written exhaustive continuation search on **every gravity-reachable state** of a 4×3 Connect-3 board, allowing play to continue after wins in both implementations so the check matches the relaxed semantics of the lemma.

- 12,304 distinct gravity-reachable states were enumerated.
- 14 candidate lines were checked for each of the two target players.
- **344,512 exact DP-versus-brute-force comparisons** were performed.
- All comparisons agreed.

This verifies the implementation over that finite domain. The uniform lemma above, not the finite test, is what supports applying the filter to 9×7.

## Result D — a 9×7 example where GTR proves a line impossible

The script contains a legal, nonterminal 40-ply history on the actual 9×7 board. In the resulting state:

- it is player X's turn;
- columns 5, 6, 7, and 8 are full;
- columns 0 through 4 have heights `2, 1, 3, 4, 2`;
- the five cells across the **top row** of columns 0 through 4 are all empty;
- player O does not already have any token in those five target cells.

Nevertheless, O can never complete that top horizontal line. To reach its five top-row cells requires respectively `5 + 6 + 4 + 3 + 5 = 23` drops in columns 0–4. There is no spare capacity in the other four columns. Since X moves first from this position, the 23rd drop belongs to X; and the 23rd drop must be one of the five target top-row cells. Therefore O cannot be the player who completes all five cells. GTR rejects this line without searching the full game tree.

This is a concrete parity-plus-gravity obstruction, not a heuristic score.

The resulting board (top row first; `X` is player 0 and `O` is player 1) is:

```text
. . . . . O X X X
. . . . . X X O O
. . . . . X X O X
. . . O . O X X O
. . O X . X O X X
O . O O O X O X X
O X X O O X O O O
```

The target line is the five dots in columns 0–4 of the top row.

## Result E — initial state counts and symmetry quotient

Exact state enumeration on 9×7 through ply 8 produced the following counts. No win is possible before ply 9, so every state in these layers is an ordinary legal game state.

| Ply | Distinct positions | After left-right symmetry |
|---:|---:|---:|
| 0 | 1 | 1 |
| 1 | 9 | 5 |
| 2 | 81 | 41 |
| 3 | 477 | 241 |
| 4 | 2,745 | 1,385 |
| 5 | 12,285 | 6,161 |
| 6 | 55,989 | 28,025 |
| 7 | 214,695 | 107,423 |
| 8 | 844,704 | 422,572 |

These counts are useful regression targets for the phase-2 state generator. They are not an estimate of the full game's total state space.

## Artifacts and reproducibility

- `gpc_phase1.py` implements the line generator, mirror canonicalization, GTR dynamic program, independent brute-force oracle, and the complete self-test.
- Run with: `python3 gpc_phase1.py`.
- Successful run prints a JSON summary and exits with status 0.

## Limits — what is not claimed

- The initial 9×7 Connect-5 game has **not** been solved in this phase.
- No winner/draw value for the initial position has been established.
- GTR is a necessary-condition filter for a single candidate line, not a strategy proof.
- The finite 4×3 validation does not replace the lemma proof and does not constitute exhaustive validation of the target 9×7 game.
- The model assumes the first completed five-in-a-row ends the game and a full board without a line is a draw.

## Three-stage program boundary

1. **Phase 1 — completed:** formal state geometry, exact reflection quotient, sound line-reachability pruning, and independent finite validation.
2. **Phase 2 — next, not started:** derive and prove compound tactical rules from GTR (multi-line dependencies, forced replies, double threats), and integrate them into a proof-producing AND/OR search. Every rule must carry explicit preconditions and a verifier.
3. **Phase 3 — not started:** partition unresolved root branches, execute the proof search on external compute, merge independently verifiable certificates, and determine the initial position's game-theoretic value.
