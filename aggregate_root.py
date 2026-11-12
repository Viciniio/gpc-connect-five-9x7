#!/usr/bin/env python3
"""Mirror-expand 82 symmetry-unique DFPN obligations and verify root strategy DAGs."""
from __future__ import annotations
import argparse, copy, hashlib, json, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PHASE2 = ROOT / "phase2"
if str(PHASE2) not in sys.path:
    sys.path.insert(0, str(PHASE2))
from gpc_phase2 import Game, encode_state, verify_certificate

W,H,K=9,7,5


def read_results(folder: Path):
    results={}
    for path in folder.glob("result-t*-f*-s*.json"):
        data=json.loads(path.read_text(encoding="utf-8"))
        key=(int(data["target"]), int(data["history"][0]), int(data["history"][1]))
        results[key]=data
    return results


def representative(first: int, second: int):
    """Return the unique representative and whether its proof must be mirrored."""
    if first > 4:
        return 8-first, 8-second, True
    if first == 4 and second > 4:
        return 4, 8-second, True
    return first, second, False


def mirror_bits(bits: int):
    out=0
    for c in range(W):
        for r in range(H):
            if bits & (1 << (c*H+r)):
                out |= 1 << ((W-1-c)*H+r)
    return out


def mirror_certificate(cert: dict, game: Game):
    """Reflect a verified certificate through the exact board automorphism."""
    out=copy.deepcopy(cert)
    out["root_history"]=[W-1-c for c in out.get("root_history",[])]
    for node in out["nodes"]:
        state=node["state"]
        state["p0"]=hex(mirror_bits(int(state["p0"],16)))
        state["p1"]=hex(mirror_bits(int(state["p1"],16)))
        state["heights"]=list(reversed(state["heights"]))
        for field in ("move","blocking_column","next_winning_column"):
            if field in node and node[field] is not None:
                node[field]=W-1-int(node[field])
        if "winning_columns" in node:
            node["winning_columns"]=sorted(W-1-int(c) for c in node["winning_columns"])
        if "children" in node:
            for edge in node["children"]:
                edge["move"]=W-1-int(edge["move"])
    payload={k:v for k,v in out.items() if k!="sha256"}
    out["sha256"]=hashlib.sha256(json.dumps(payload,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    ok,msg=verify_certificate(out)
    if not ok:
        raise ValueError("horizontally reflected certificate failed verification: "+msg)
    return out


def read_branch_cert(folder: Path, target: int, first: int, second: int, claim: str, game: Game):
    rep_first,rep_second,flipped=representative(first,second)
    p=folder/f"cert-t{target}-f{rep_first}-s{rep_second}-{claim}.json"
    if not p.exists(): return None
    cert=json.loads(p.read_text(encoding="utf-8"))
    if cert.get("target")!=target or cert.get("claim")!=claim or cert.get("root_history")!=[rep_first,rep_second]:
        raise ValueError(f"branch identity mismatch: {p.name}")
    ok,msg=verify_certificate(cert)
    if not ok: raise ValueError(f"branch certificate rejected {p.name}: {msg}")
    return mirror_certificate(cert,game) if flipped else cert


def add_branch(nodes, cert):
    offset=len(nodes)
    for raw in cert["nodes"]:
        n=dict(raw)
        n["id"] += offset
        if n["kind"] in ("OR","OR_NOT_WIN"):
            n["child"] += offset
        elif n["kind"] in ("AND","AND_NOT_WIN"):
            n["children"]=[{**edge,"child":edge["child"]+offset} for edge in n["children"]]
        nodes.append(n)
    return cert["root"]+offset


def build_root_cert(game: Game, target: int, claim: str, branch):
    # branch[(first,second)] -> verified branch certificate; all 81 logical
    # opening pairs are present after horizontal symmetry expansion.
    start=(0,0,game.empty_heights,0)
    nodes=[]

    def attach(first, second):
        cert=branch.get((first,second))
        if cert is None: raise ValueError("required branch certificate is unavailable")
        return add_branch(nodes,cert)

    if target==0 and claim=="target_win":
        chosen=next((a for a in range(W) if all((a,b) in branch for b in range(W))),None)
        if chosen is None:return None
        nodes.extend([None,None])
        edges=[{"move":b,"child":attach(chosen,b)} for b in range(W)]
        nodes[1]={"id":1,"kind":"AND","state":encode_state(game.play(start,chosen)),
                  "target":target,"children":edges,"claim":claim}
        nodes[0]={"id":0,"kind":"OR","state":encode_state(start),"target":target,
                  "move":chosen,"child":1,"claim":claim}
    elif target==1 and claim=="target_win":
        choices={}
        for a in range(W):
            b=next((b for b in range(W) if (a,b) in branch),None)
            if b is None:return None
            choices[a]=b
        nodes.extend([None]*(1+W)); edges=[]
        for idx,a in enumerate(range(W),1):
            b=choices[a]; child_id=attach(a,b)
            nodes[idx]={"id":idx,"kind":"OR","state":encode_state(game.play(start,a)),
                        "target":target,"move":b,"child":child_id,"claim":claim}
            edges.append({"move":a,"child":idx})
        nodes[0]={"id":0,"kind":"AND","state":encode_state(start),"target":target,
                  "children":edges,"claim":claim}
    elif target==0 and claim=="target_not_win":
        choices={}
        for a in range(W):
            b=next((b for b in range(W) if (a,b) in branch),None)
            if b is None:return None
            choices[a]=b
        nodes.extend([None]*(1+W)); edges=[]
        for idx,a in enumerate(range(W),1):
            b=choices[a]; child_id=attach(a,b)
            nodes[idx]={"id":idx,"kind":"OR_NOT_WIN","state":encode_state(game.play(start,a)),
                        "target":target,"move":b,"child":child_id,"claim":claim}
            edges.append({"move":a,"child":idx})
        nodes[0]={"id":0,"kind":"AND_NOT_WIN","state":encode_state(start),"target":target,
                  "children":edges,"claim":claim}
    elif target==1 and claim=="target_not_win":
        chosen=next((a for a in range(W) if all((a,b) in branch for b in range(W))),None)
        if chosen is None:return None
        nodes.extend([None,None])
        edges=[{"move":b,"child":attach(chosen,b)} for b in range(W)]
        nodes[1]={"id":1,"kind":"AND_NOT_WIN","state":encode_state(game.play(start,chosen)),
                  "target":target,"children":edges,"claim":claim}
        nodes[0]={"id":0,"kind":"OR_NOT_WIN","state":encode_state(start),"target":target,
                  "move":chosen,"child":1,"claim":claim}
    else:
        raise ValueError("unsupported root target/claim")

    cert={"format":"GPC-ANDOR-CERT-1","game":{"width":W,"height":H,"k":K},
          "root":0,"target":target,"claim":claim,"root_history":[],"nodes":nodes}
    cert["sha256"]=hashlib.sha256(json.dumps(cert,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    ok,msg=verify_certificate(cert)
    if not ok:raise ValueError(f"root certificate failed independent verification: {msg}")
    return cert,msg


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--results",type=Path,required=True)
    ap.add_argument("--output",type=Path,required=True)
    args=ap.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    game=Game.make(W,H,K)
    results=read_results(args.results)
    expected_physical=82
    branch={0:{"target_win":{},"target_not_win":{}},1:{"target_win":{},"target_not_win":{}}}
    counts={"target_win":0,"target_not_win":0,"unresolved":0,"missing":0}
    logical_reported=0
    for target in range(2):
        for first in range(W):
            for second in range(W):
                rf,rs,_=representative(first,second)
                result=results.get((target,rf,rs))
                if result is None:
                    counts["missing"]+=1;continue
                logical_reported+=1
                status=result.get("status","unresolved")
                if status not in ("target_win","target_not_win"):
                    counts["unresolved"]+=1;continue
                cert=read_branch_cert(args.results,target,first,second,status,game)
                if cert is None:
                    counts["unresolved"]+=1;continue
                branch[target][status][(first,second)]=cert
                counts[status]+=1

    root_certs={}
    for target in range(2):
        for claim in ("target_win","target_not_win"):
            try:
                built=build_root_cert(game,target,claim,branch[target][claim])
            except Exception as exc:
                built=None
                (args.output/f"error-target{target}-{claim}.txt").write_text(str(exc),encoding="utf-8")
            if built:
                cert,msg=built
                path=args.output/f"root-target{target}-{claim}.json"
                path.write_text(json.dumps(cert,separators=(",",":")),encoding="utf-8")
                root_certs[(target,claim)]={"path":path.name,"nodes":len(cert["nodes"]),"verification":msg}

    xwin=(0,"target_win") in root_certs
    owin=(1,"target_win") in root_certs
    xnot=(0,"target_not_win") in root_certs
    onot=(1,"target_not_win") in root_certs
    if xwin and owin:outcome="CONTRADICTORY_CERTIFICATES"
    elif xwin:outcome="PLAYER_1_FORCED_WIN"
    elif owin:outcome="PLAYER_2_FORCED_WIN"
    elif xnot and onot:outcome="DRAW"
    else:outcome="UNRESOLVED"
    summary={"format":"GPC-ROOT-AGGREGATE-1","rules":{"width":9,"height":7,"connect":5},
             "outcome":outcome,"physical_subproblems_expected":expected_physical,
             "physical_subproblems_reported":len(results),"logical_two_ply_branches_required":162,
             "logical_two_ply_branches_reported":logical_reported,
             "branch_certificates":counts,"root_certificates":root_certs,
             "proof_requirement":"Only a root certificate independently accepted by GPC Phase 2 verifier establishes a result."}
    (args.output/"game_value_report.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
    print(json.dumps(summary,indent=2))
    return 0 if outcome not in ("UNRESOLVED","CONTRADICTORY_CERTIFICATES") else 3

if __name__=="__main__":
    raise SystemExit(main())
