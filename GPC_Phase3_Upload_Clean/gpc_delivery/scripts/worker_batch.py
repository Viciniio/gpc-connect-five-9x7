#!/usr/bin/env python3
"""Launch four proof obligations and monitor for an independently verified root proof."""
from __future__ import annotations
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def tasks():
    # Horizontal reflection reduces the 9 first moves to five representatives.
    # For the center opening, the second reply also has mirror pairs.
    return [(target, first, second)
            for target in (0, 1)
            for first in range(5)
            for second in (range(9) if first &lt; 4 else range(5))]

def main():
 ap = argparse.ArgumentParser()
 ap.add_argument("--group", type=int, required=True)
 ap.add_argument("--budget-seconds", type=int, default=18400)
 ap.add_argument("--slice-ms", type=int, default=120000)
 ap.add_argument("--node-limit", type=int, default=50000000)
 ap.add_argument("--max-entries", type=int, default=4000000)
 args = ap.parse_args()

 all_tasks = tasks()
 group = all_tasks[args.group * 4:(args.group + 1) * 4]
    if not group:
        return 0

 out = ROOT / "out"
 out.mkdir(parents=True, exist_ok=True)
 stop_file = out / ("root-proof-found-" + str(args.group) + ".json")
    if stop_file.exists():
 stop_file.unlink()

 monitor = subprocess.Popen([
 sys.executable, str(ROOT / "scripts" / "early_root_monitor.py"),
        "--group", str(args.group), "--interval", "300",
        "--stop-file", str(stop_file)
    ], cwd=ROOT)

 commands = []
    for target, first, second in group:
 commands.append([
 sys.executable, str(ROOT / "scripts" / "run_worker.py"),
            "--target", str(target), "--first", str(first), "--second", str(second),
            "--budget-seconds", str(args.budget_seconds),
            "--slice-ms", str(args.slice_ms),
            "--node-limit", str(args.node_limit),
            "--max-entries", str(args.max_entries),
            "--out", str(out)
        ])

 procs = [subprocess.Popen(cmd, cwd=ROOT) for cmd in commands]
    try:
        while any(p.poll() is None for p in procs):
            if stop_file.exists():
                print("ROOT-PROOF VERIFIED; stopping remaining local workers.", flush=True)
                for p in procs:
                    if p.poll() is None:
 p.terminate()
                break
 time.sleep(2)
    finally:
        if monitor.poll() is None:
 monitor.terminate()
        try:
 monitor.wait(timeout=15)
        except subprocess.TimeoutExpired:
 monitor.kill()
 monitor.wait()

 codes = [p.wait() for p in procs]
    if stop_file.exists():
        print("Early stop is backed by a verified root certificate.", flush=True)
        return 0
    if monitor.returncode not in (0, -15, None):
        print(f"Root monitor exited with code {monitor.returncode}.", file=sys.stderr)
    if any(c != 0 for c in codes):
        print(f"group {args.group} had worker exits: {codes}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
