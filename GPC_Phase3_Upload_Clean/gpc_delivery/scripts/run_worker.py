#!/usr/bin/env python3
"""Resume one fixed two-ply DFPN obligation and publish durable checkpoint slots."""
from __future__ import annotations
import argparse, json, os, shutil, subprocess, sys, threading, time
from pathlib import Path

TAG = "gpc-phase3-checkpoints"
UPLOAD_INTERVAL = 300
ROOT = Path(__file__).resolve().parents[1]


def run(cmd, check=False, capture=False):
    return subprocess.run(cmd, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


def gh_download(asset: str, dest: Path) -> bool:
    dest.mkdir(parents=True, exist_ok=True)
    r=run(["gh","release","download",TAG,"--pattern",asset,"--dir",str(dest),"--clobber"],capture=True)
    return r.returncode==0 and (dest/asset).exists()


def gh_upload(path: Path, asset_name: str|None=None) -> bool:
    if not path.exists(): return False
    args=["gh","release","upload",TAG,str(path),"--clobber"]
    if asset_name and asset_name!=path.name:
        # Upload the file under a stable, unambiguous release-asset name.
        tmp=path.parent/asset_name
        if tmp!=path:
            shutil.copy2(path,tmp)
            args=["gh","release","upload",TAG,str(tmp),"--clobber"]
            r=run(args,capture=True)
            try: tmp.unlink()
            except OSError: pass
            return r.returncode==0
    return run(args,capture=True).returncode==0


def verifier_ok(cert_path: Path) -> bool:
    script=ROOT/"phase2"/"gpc_phase2.py"
    # Verify through the independent Phase-2 verifier, not the DFPN engine.
    code=("import json,sys; from pathlib import Path; "
          "sys.path.insert(0, str(Path(sys.argv[1]).resolve().parent)); "
          "from gpc_phase2 import verify_certificate; "
          "c=json.loads(Path(sys.argv[2]).read_text()); ok,msg=verify_certificate(c); "
          "print(msg); sys.exit(0 if ok else 1)")
    return run([sys.executable,"-c",code,str(script),str(cert_path)],capture=True).returncode==0


def prepare_certificate(target: int, first: int, second: int, solved_path: Path,
                        result_path: Path, out_dir: Path):
    data=json.loads(result_path.read_text(encoding="utf-8"))
    claim=data.get("status")
    if claim not in ("target_win","target_not_win"):
        return None
    cert=out_dir/f"cert-t{target}-f{first}-s{second}-{claim}.json"
    cmd=[sys.executable,str(ROOT/"scripts"/"make_certificate_from_dfpn.py"),
         "--solved",str(solved_path),"--target",str(target),"--claim",claim,
         "--history",str(first),str(second),"--output",str(cert)]
    p=run(cmd,capture=True)
    if p.returncode!=0 or not cert.exists():
        data["certificate_ok"]=False
        data["certificate_error"]=(p.stdout or "")+(p.stderr or "")
        result_path.write_text(json.dumps(data,indent=2),encoding="utf-8")
        return None
    if not verifier_ok(cert):
        data["certificate_ok"]=False
        data["certificate_error"]="Independent GPC Phase-2 verifier rejected certificate"
        result_path.write_text(json.dumps(data,indent=2),encoding="utf-8")
        return None
    data["certificate_ok"]=True
    data["certificate_file"]=cert.name
    result_path.write_text(json.dumps(data,indent=2),encoding="utf-8")
    return cert


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--target",type=int,choices=(0,1),required=True)
    ap.add_argument("--first",type=int,choices=range(9),required=True)
    ap.add_argument("--second",type=int,choices=range(9),required=True)
    ap.add_argument("--budget-seconds",type=int,default=18400)
    ap.add_argument("--slice-ms",type=int,default=120000)
    ap.add_argument("--node-limit",type=int,default=50_000_000)
    ap.add_argument("--max-entries",type=int,default=4_000_000)
    ap.add_argument("--binary",type=Path,default=ROOT/"build"/"gpc_worker")
    ap.add_argument("--out",type=Path,default=ROOT/"out")
    args=ap.parse_args()
    if not os.environ.get("GH_TOKEN") and not os.environ.get("GITHUB_TOKEN"):
        raise SystemExit("GH_TOKEN/GITHUB_TOKEN is required to persist checkpoints in GitHub Releases")
    args.out.mkdir(parents=True,exist_ok=True)
    shard=f"t{args.target}-f{args.first}-s{args.second}"
    checkpoint_name=f"checkpoint-{shard}"
    manifest_name=f"manifest-{shard}.json"
    result_name=f"result-{shard}.json"
    solved_name=f"solved-{shard}.txt"
    local_checkpoint=args.out/(checkpoint_name+".bin")
    local_manifest=args.out/manifest_name
    result_path=args.out/result_name
    solved_path=args.out/solved_name
    state_dir=args.out/"release_downloads"

    # Reuse completed proof only after re-running the independent certificate verifier.
    downloaded_result=gh_download(result_name,state_dir)
    downloaded_solved=gh_download(solved_name,state_dir)
    downloaded_cert_names=[]
    if downloaded_result:
        shutil.copy2(state_dir/result_name,result_path)
        prev=json.loads(result_path.read_text(encoding="utf-8"))
        claim=prev.get("status")
        if claim in ("target_win","target_not_win") and downloaded_solved:
            shutil.copy2(state_dir/solved_name,solved_path)
            cert=prepare_certificate(args.target,args.first,args.second,solved_path,result_path,args.out)
            if cert:
                gh_upload(cert)
                gh_upload(result_path)
                print(f"SKIP already-solved {shard}: independently verified {claim}",flush=True)
                return 0

    # Load the latest committed checkpoint via a two-slot manifest. The previous
    # active slot is never deleted until the replacement slot and manifest are uploaded.
    if gh_download(manifest_name,state_dir):
        shutil.copy2(state_dir/manifest_name,local_manifest)
        manifest=json.loads(local_manifest.read_text(encoding="utf-8"))
        slot=manifest.get("active_slot")
        asset=manifest.get("asset")
        if slot in ("A","B") and isinstance(asset,str) and gh_download(asset,state_dir):
            shutil.copy2(state_dir/asset,local_checkpoint)
            print(f"RESUME {shard} generation={manifest.get('generation')} slot={slot}",flush=True)
    if not local_checkpoint.exists():
        print(f"START new proof obligation {shard}",flush=True)

    stop=threading.Event()
    upload_lock=threading.Lock()
    manifest={"format":"GPC-CHECKPOINT-MANIFEST-1","generation":0,"active_slot":None,"asset":None}
    if local_manifest.exists():
        try: manifest=json.loads(local_manifest.read_text(encoding="utf-8"))
        except Exception: pass
    last_uploaded_mtime=local_checkpoint.stat().st_mtime_ns if local_checkpoint.exists() else -1

    def upload_checkpoint(force=False):
        nonlocal manifest,last_uploaded_mtime
        if not local_checkpoint.exists(): return
        mtime=local_checkpoint.stat().st_mtime_ns
        if not force and mtime==last_uploaded_mtime: return
        with upload_lock:
            # Rotate to the inactive slot; upload the new bytes first, then move the pointer.
            inactive="B" if manifest.get("active_slot")=="A" else "A"
            asset=f"{checkpoint_name}.{inactive}.bin"
            if not gh_upload(local_checkpoint,asset):
                print(f"WARN checkpoint asset upload failed for {shard}",flush=True)
                return
            next_manifest={"format":"GPC-CHECKPOINT-MANIFEST-1",
                           "generation":int(manifest.get("generation",0))+1,
                           "active_slot":inactive,"asset":asset,
                           "target":args.target,"first":args.first,"second":args.second}
            tmp=args.out/(manifest_name+".tmp")
            tmp.write_text(json.dumps(next_manifest,separators=(",",":")),encoding="utf-8")
            if gh_upload(tmp,manifest_name):
                manifest=next_manifest
                local_manifest.write_text(json.dumps(manifest,indent=2),encoding="utf-8")
                last_uploaded_mtime=mtime
                print(f"CHECKPOINT {shard} generation={manifest['generation']}",flush=True)
            else:
                print(f"WARN checkpoint manifest upload failed for {shard}",flush=True)
            try: tmp.unlink()
            except OSError: pass

    def uploader():
        while not stop.wait(UPLOAD_INTERVAL):
            upload_checkpoint(False)
            # Publish the latest slice summary so root proof can be assembled early.
            if result_path.exists():
                if not gh_upload(result_path):
                    print(f"WARN progress result upload failed for {shard}", flush=True)

    thread=threading.Thread(target=uploader,daemon=True)
    thread.start()
    moves=[chr(ord("a")+args.first),chr(ord("a")+args.second)]
    cmd=[str(args.binary),"--distributed-worker",str(args.target),str(args.budget_seconds),
         str(args.slice_ms),str(args.node_limit),str(args.max_entries),str(local_checkpoint),
         str(solved_path),str(result_path),*moves]
    log_path=args.out/f"log-{shard}.txt"
    try:
        with log_path.open("w",encoding="utf-8") as log:
            proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,cwd=ROOT)
            code=proc.wait()
    finally:
        stop.set(); thread.join(timeout=30)
    upload_checkpoint(True)
    if not solved_path.exists() or not result_path.exists():
        print(f"ERROR worker did not produce its summary for {shard}",flush=True)
        return 4
    cert=prepare_certificate(args.target,args.first,args.second,solved_path,result_path,args.out)
    if cert:
        gh_upload(cert)
    gh_upload(solved_path)
    gh_upload(result_path)
    summary=json.loads(result_path.read_text(encoding="utf-8"))
    print(f"COMPLETE {shard} status={summary.get('status')} certificate_ok={summary.get('certificate_ok',False)} exit={code}",flush=True)
    return 0 if code==0 else code

if __name__=="__main__":
    raise SystemExit(main())
