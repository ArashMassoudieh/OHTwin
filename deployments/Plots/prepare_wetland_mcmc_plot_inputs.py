#!/usr/bin/env python3
"""Normalize Wetland truth/MCMC/reanalysis outputs for robust Gnuplot use."""
from __future__ import annotations
import argparse, csv, re, sys
from pathlib import Path

VARIABLES = {
    "precip": ["precipitation"],
    "inflow": ["wetland inflow"],
    "inlet_stage": ["wetland inlet stage", "inlet stage"],
    "mid_stage": ["wetland mid-stage", "wetland mid stage", "mid-stage", "mid stage"],
    "outlet_stage": ["wetland outlet stage", "outlet stage"],
    "outflow": ["wetland outflow", "outflow"],
    "cell1_depth": ["cell 1 water depth", "cell 1 depth", "cell1"],
    "cell6_depth": ["cell 6 water depth", "cell 6 depth", "cell6"],
    "hrt": ["hrt"],
}
OUT_HEADER = ["time"] + list(VARIABLES)

def clean(s: str) -> str:
    s = re.sub(r"\([^)]*\)", "", s.strip().lower()).replace("_", " ")
    return re.sub(r"\s+", " ", s).strip()

def find_var_indices(header: list[str]) -> dict[str, tuple[int, int]]:
    h = [clean(x) for x in header]
    found = {}
    for var, keys in VARIABLES.items():
        for j, name in enumerate(h):
            if name == "t" or not name:
                continue
            if any(k in name for k in keys):
                found[var] = (j - 1 if j > 0 and h[j - 1] == "t" else 0, j)
                break
    return found

def normalize(src: Path, dst: Path, strict: bool) -> None:
    if not src.exists():
        print(f"[warn] missing input: {src}", file=sys.stderr)
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open(newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        header = next(r, None)
        if header is None:
            raise ValueError(f"empty file: {src}")
        idx = find_var_indices(header)
        missing = [v for v in VARIABLES if v not in idx]
        if missing:
            msg = f"{src}: missing variables: {missing}; header={header}"
            if strict:
                raise KeyError(msg)
            print(f"[warn] {msg}", file=sys.stderr)
        t_idx = (idx.get("inlet_stage") or next(iter(idx.values()), (0, 0)))[0]
        with dst.open("w", newline="", encoding="utf-8") as out:
            w = csv.writer(out); w.writerow(OUT_HEADER)
            for row in r:
                if not row: continue
                values = [row[t_idx] if t_idx < len(row) else "nan"]
                for var in VARIABLES:
                    j = idx[var][1] if var in idx else -1
                    values.append(row[j] if 0 <= j < len(row) else "nan")
                w.writerow(values)
    print(f"[ok] wrote {dst}", file=sys.stderr)

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--truth", type=Path, default=Path("Wetland_truth/outputs/selected_output.csv"))
    p.add_argument("--mcmc", type=Path, default=Path("Wetland_assimilation_MCMC/outputs/selected_output.csv"))
    p.add_argument("--reanalysis", type=Path, default=Path("Wetland_assimilation_MCMC/outputs/reanalysis_output.csv"))
    p.add_argument("--outdir", type=Path, default=Path("Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc"))
    p.add_argument("--strict", action="store_true")
    a = p.parse_args()
    normalize(a.truth, a.outdir / "truth_normalized_mcmc.csv", a.strict)
    normalize(a.mcmc, a.outdir / "mcmc_normalized.csv", a.strict)
    normalize(a.reanalysis, a.outdir / "reanalysis_normalized_mcmc.csv", a.strict)
    return 0
if __name__ == "__main__": raise SystemExit(main())
