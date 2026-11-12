#!/usr/bin/env python3
"""Launch symmetry-unique proof obligations on one 4-vCPU GitHub runner."""
from __future__ import annotations
import argparse, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def tasks():
    # Horizontal reflection reduces the 9 first moves to five representatives.
    # For the fixed center opening, the second reply also has mirror pairs.
    return [(target, first, second)
            for target in (0, 1)
            for first in range(5)
            for second in (range(9) if first < 4 else range(5))]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--group",type=int,required=True)
    ap.add_argument("--budget-seconds",type=int,default=18400)
    ap.add_argument("--slice-ms",type=int,default=120000)
    ap.add_argument("--node-limit",type=int,default=50_000_000)
    ap.add_argument("--max-entries",type=int,default=4_000_000)
    args=ap.parse_args()
    all_tasks=tasks()
    group=all_tasks[args.group*4:(args.group+1)*4]
    if not group:
        return 0
    commands=[]
    for target,first,second in group:
        commands.append([sys.executable,str(ROOT/"scripts"/"run_worker.py"),
                         "--target",str(target),"--first",str(first),"--second",str(second),
                         "--budget-seconds",str(args.budget_seconds),"--slice-ms",str(args.slice_ms),
                         "--node-limit",str(args.node_limit),"--max-entries",str(args.max_entries),
                         "--out",str(ROOT/"out")])
    procs=[subprocess.Popen(cmd,cwd=ROOT) for cmd in commands]
    codes=[p.wait() for p in procs]
    if any(c!=0 for c in codes):
        print(f"group {args.group} had worker exits: {codes}",file=sys.stderr)
        return 1
    return 0
if __name__=="__main__":
    raise SystemExit(main())
