# Gravity Connect-5 (9×7) proof-search prototype

This C++17 program applies depth-first proof-number search (DFPN) to the *normal-play* gravity game: players alternate dropping one stone into a non-full column, and the first player to make five consecutive stones horizontally, vertically, or diagonally wins. It uses horizontal-reflection symmetry and a fixed-size transposition table.

## Build

```bash
g++ -O3 -march=native -std=c++17 connect5_gravity_proof_search.cpp -o connect5_solver
```

## Run the 9×7 opening position

```bash
./connect5_solver 9 7 5 0 600 22 -1 1
```

Arguments, in order:

1. width (`9`)
2. height (`7`)
3. stones in a row to win (`5`)
4. node limit (`0` means no node limit)
5. time limit in seconds (`0` means no time limit)
6. transposition-table size exponent (`22` allocates roughly 512 MiB; larger values need more RAM)
7. pre-played first move column, 0-based (`-1` means the board is empty; use `0` through `8` to analyze a specific first move by X)
8. target player (`1` for X / first player; `2` for O / second player)

## Interpreting the output

- `SOLVED target=1 CAN_FORCE_WIN`: the search proved X has a forced win from the chosen start position.
- `SOLVED target=2 CAN_FORCE_WIN`: the search proved O has a forced win from that start position.
- `CANNOT_FORCE_WIN`: only means the selected target cannot force a win; to distinguish a forced loss from a draw, run the search for the other target too.
- `UNSOLVED_LIMIT`: no game-theoretic result was proved within the limit. The printed PN/DN numbers are search state, **not** a prediction or a proof of the winner.

The program does not emit a separately checkable proof certificate; an external verifier would be a worthwhile next step if the root result is solved.