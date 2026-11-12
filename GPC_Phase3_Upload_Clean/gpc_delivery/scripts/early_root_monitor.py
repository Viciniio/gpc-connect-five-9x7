#!/usr/bin/env python3
"""Stop unnecessary GPC work only after a verified root certificate exists."""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

TAG = "gpc-phase3-checkpoints"
ROOT = Path(__file__).resolve().parents[1]
VALID = {"PLAYER_1_FORCED_WIN", "PLAYER_2_FORCED_WIN", "DRAW"}

def run(args):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", type=int, required=True)
    ap.add_argument("--stop-file", type=Path, required=True)
    ap.add_argument("--interval", type=int, default=300)
    args = ap.parse_args()
    repo = os.environ["GITHUB_REPOSITORY"]
    run_id = os.environ["GITHUB_RUN_ID"]
    base = ROOT / "out" / ("root-monitor-" + str(args.group))
    results = base / "results"
    final = base / "final"
    results.mkdir(parents=True, exist_ok=True)
    final.mkdir(parents=True, exist_ok=True)
    seen = {}

    while not args.stop_file.exists():
        meta = run(["gh", "api", "repos/" + repo + "/releases/tags/" + TAG])
        if meta.returncode:
            time.sleep(args.interval)
            continue
        try:
            assets = json.loads(meta.stdout).get("assets", [])
        except Exception:
            time.sleep(args.interval)
            continue

        changed = []
        for asset in assets:
            name = asset.get("name", "")
            stamp = asset.get("updated_at", "")
            if name.startswith(("result-", "cert-")) and seen.get(name) != stamp:
                changed.append((name, stamp))

        got_new = False
        for name, stamp in changed:
            dl = run(["gh", "release", "download", TAG, "--pattern", name,
                      "--dir", str(results), "--clobber"])
            if dl.returncode == 0 and (results / name).exists():
                seen[name] = stamp
                got_new = True

        if got_new:
            run([sys.executable, str(ROOT / "scripts" / "aggregate_root.py"),
                 "--results", str(results), "--output", str(final)])
            report_file = final / "game_value_report.json"
            if report_file.exists():
                try:
                    report = json.loads(report_file.read_text(encoding="utf-8"))
                except Exception:
                    report = {}
                outcome = report.get("outcome")
                if outcome in VALID:
                    files = sorted(final.glob("root-target*.json")) + [report_file]
                    upload = run(["gh", "release", "upload", TAG,
                                  *[str(p) for p in files], "--clobber"])
                    if upload.returncode == 0:
                        args.stop_file.parent.mkdir(parents=True, exist_ok=True)
                        args.stop_file.write_text(json.dumps({
                            "outcome": outcome,
                            "root_certificates": report.get("root_certificates", {}),
                            "run_id": run_id
                        }, indent=2), encoding="utf-8")
                        run(["gh", "run", "cancel", run_id, "--repo", repo])
                        return 0
        time.sleep(max(60, args.interval))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
