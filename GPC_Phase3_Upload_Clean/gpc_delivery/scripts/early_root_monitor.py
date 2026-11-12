#!/usr/bin/env python3
"""Early root-proof monitor for distributed GPC Phase 3 workers."""
from __future__ import annotations
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

TAG = "gpc-phase3-checkpoints"
ROOT = Path(__file__).resolve().parents[1]
AGGREGATOR = ROOT / "scripts" / "aggregate_root.py"
RESOLVED = {"PLAYER_1_FORCED_WIN", "PLAYER_2_FORCED_WIN", "DRAW"}

def command(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, text=True,
 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
 check=False)

def monitor(group: int, interval: int, stop_file: Path) -> int:
 repo = os.environ.get("GITHUB_REPOSITORY", "")
 run_id = os.environ.get("GITHUB_RUN_ID", "")
    if not repo or not run_id:
        print("ROOT-MONITOR: missing GitHub run environment", flush=True)
        return 2
 base = ROOT / "out" / f"root-monitor-{group}"
 results_dir = base / "results"
 output_dir = base / "final"
 results_dir.mkdir(parents=True, exist_ok=True)
 output_dir.mkdir(parents=True, exist_ok=True)
 seen: dict[str, str] = {}
    print(f"ROOT-MONITOR group={group} interval={interval}s", flush=True)

    while not stop_file.exists():
 meta = command(["gh", "api", f"repos/{repo}/releases/tags/{TAG}"])
        if meta.returncode != 0:
            print("ROOT-MONITOR: release metadata unavailable; retrying", flush=True)
 time.sleep(interval)
            continue
        try:
 assets = json.loads(meta.stdout).get("assets", [])
        except Exception as exc:
            print(f"ROOT-MONITOR: invalid release metadata: {exc}", flush=True)
 time.sleep(interval)
            continue

 changed = []
        for asset in assets:
 ame = asset.get("name", "")
 updated = asset.get("updated_at", "")
            if ame.startswith(("result-", "cert-")) and seen.get( ame) != updated:
 changed.append(( ame, updated))

 downloaded_any = False
        for ame, updated in changed:
 dl = command(["gh", "release", "download", TAG, "--pattern", name,
                          "--dir", str(results_dir), "--clobber"])
            if dl.returncode == 0 and (results_dir / name).exists():
 seen[ ame] = updated
 downloaded_any = True
            else:
                print(f"ROOT-MONITOR: download pending for { ame}", flush=True)

        if downloaded_any:
 agg = command([sys.executable, str(AGGREGATOR), "--results",
                           str(results_dir), "--output", str(output_dir)])
 report_path = output_dir / "game_value_report.json"
            if report_path.exists():
                try:
 report = json.loads(report_path.read_text(encoding="utf-8"))
                except Exception:
 report = {}
 outcome = report.get("outcome", "UNRESOLVED")
                if outcome in RESOLVED:
 proof_files = sorted(output_dir.glob("root-target*.json"))
 proof_files.append(report_path)
 upload = command(["gh", "release", "upload", TAG,
                                      *[str(p) for p in proof_files], "--clobber"])
                    if upload.returncode == 0:
 stop_file.parent.mkdir(parents=True, exist_ok=True)
 stop_file.write_text(json.dumps({
                            "outcome": outcome,
                            "root_certificates": report.get("root_certificates", {}),
                            "branch_certificates": report.get("branch_certificates", {}),
                            "run_id": run_id,
                        }, indent=2), encoding="utf-8")
                        print(f"ROOT-PROOF VERIFIED: {outcome}", flush=True)
 cancel = command(["gh", "run", "cancel", run_id, "--repo", repo])
                        print(f"ROOT-MONITOR: cancellation requested (exit={cancel.returncode})",
 flush=True)
                        return 0
                    print("ROOT-MONITOR: root proof upload failed; retrying", flush=True)
                    print(upload.stdout[-3000:], flush=True)
                elif outcome == "CONTRADICTORY_CERTIFICATES":
                    print("ROOT-MONITOR: contradictory certificates; refusing publication",
 flush=True)
                else:
                    print("ROOT-MONITOR: root remains unresolved "
                          f"(branches={report.get('logical_two_ply_branches_reported', 0)})",
 flush=True)
            elif agg.returncode not in (0, 3):
                print("ROOT-MONITOR: aggregation failed; retrying", flush=True)
                print(agg.stdout[-3000:], flush=True)
 time.sleep(interval)
    return 0

def main() -> int:
 parser = argparse.ArgumentParser()
 parser.add_argument("--group", type=int, required=True)
 parser.add_argument("--interval", type=int, default=300)
 parser.add_argument("--stop-file", type=Path, required=True)
 args = parser.parse_args()
    return monitor(args.group, max(60, args.interval), args.stop_file)

if __name__ == "__main__":
    raise SystemExit(main())
