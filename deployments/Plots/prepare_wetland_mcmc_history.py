#!/usr/bin/env python3
"""Convert streaming-MCMC JSONL/CI outputs into fixed, publication-friendly CSVs."""
from __future__ import annotations
import argparse, csv, json, math, sys
from pathlib import Path

PHYSICAL = ["CatchmentRunoffCoeff", "WetlandOutletAlpha", "PondAlphaMultiplier", "Evap_Coefficient", "Soil_Hydraulic_Conductivity"]
SIGMAS = ["Stage_Std", "Outflow_Std"]

def finite(v):
    try:
        x=float(v); return x if math.isfinite(x) else float("nan")
    except Exception: return float("nan")

def parse_history(src: Path, out: Path):
    if not src.exists():
        print(f"[warn] missing {src}", file=sys.stderr); return
    rows=[]
    with src.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            line=line.strip()
            if not line: continue
            try: r=json.loads(line)
            except json.JSONDecodeError: continue
            rows.append([r.get("cycle"), r.get("t_now"), int(bool(r.get("converged",False))),
                         r.get("ess"), r.get("pool_size"), r.get("plateaued_fraction"),
                         r.get("acceptance_rate"), r.get("sweeps"), r.get("evaluations")])
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w",newline="") as f:
        w=csv.writer(f); w.writerow(["cycle","t_now","converged","ess","pool_size","plateaued_fraction","acceptance_rate","sweeps","evaluations"]); w.writerows(rows)
    print(f"[ok] wrote {out} ({len(rows)} rows)", file=sys.stderr)

def load_ci(src: Path):
    if not src.exists():
        print(f"[warn] missing {src}", file=sys.stderr); return [],[]
    with src.open(newline="", encoding="utf-8", errors="replace") as f:
        r=csv.DictReader(f); return r.fieldnames or [], list(r)

def pick(row, name, stat):
    key=f"{name}_{stat}"
    return row.get(key, "nan")

def write_params(src: Path, out: Path, names):
    fields, rows=load_ci(src)
    out.parent.mkdir(parents=True, exist_ok=True)
    header=["cycle","t_now","converged"]
    for n in names: header += [f"{n}_mean",f"{n}_p025",f"{n}_p50",f"{n}_p975"]
    with out.open("w",newline="") as f:
        w=csv.writer(f); w.writerow(header)
        for r in rows:
            o=[r.get("cycle",""),r.get("t_now",""),r.get("converged","")]
            for n in names: o += [pick(r,n,"mean"),pick(r,n,"p025"),pick(r,n,"p50"),pick(r,n,"p975")]
            w.writerow(o)
    missing=[n for n in names if f"{n}_mean" not in fields]
    if missing: print(f"[warn] missing CI parameters in {src}: {missing}", file=sys.stderr)
    print(f"[ok] wrote {out} ({len(rows)} rows)", file=sys.stderr)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--history",type=Path,default=Path("Wetland_assimilation_MCMC/outputs/calibration/posterior_history.jsonl"))
    p.add_argument("--ci",type=Path,default=Path("Wetland_assimilation_MCMC/outputs/calibration/parameter_ci_history.csv"))
    p.add_argument("--outdir",type=Path,default=Path("Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc"))
    a=p.parse_args()
    parse_history(a.history,a.outdir/"mcmc_diagnostics_wetland.csv")
    write_params(a.ci,a.outdir/"mcmc_physical_parameters_wetland.csv",PHYSICAL)
    write_params(a.ci,a.outdir/"mcmc_sigma_parameters_wetland.csv",SIGMAS)
if __name__=="__main__": main()
