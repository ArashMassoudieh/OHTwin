#!/usr/bin/env python3
"""
Prepare normalized wetland CSVs for paper plots.

This avoids hard-coded selected_output.csv column numbers.  The OHQ selected
output format stores repeated pairs:
    t, <variable>, t, <variable>, ...
so this script reads variable names from the header and writes a compact file:
    time,precip,inflow,inlet_stage,mid_stage,outlet_stage,outflow,cell1_depth,cell6_depth,hrt

It also works for reanalysis/output files whose column order differs from the
truth/assimilation selected_output.csv, provided the header variable names are
present.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
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

OUT_HEADER = ["time"] + list(VARIABLES.keys())


def clean(s: str) -> str:
    s = s.strip().lower()
    s = re.sub(r"\([^)]*\)", "", s)  # remove units
    s = s.replace("_", " ")
    s = re.sub(r"\s+", " ", s)
    return s.strip()


def find_var_indices(header: list[str]) -> dict[str, tuple[int, int]]:
    # Returns variable -> (time_col_index, value_col_index), zero based.
    cleaned = [clean(h) for h in header]
    found: dict[str, tuple[int, int]] = {}
    for var, keys in VARIABLES.items():
        for j, h in enumerate(cleaned):
            if h == "t" or not h:
                continue
            if any(k in h for k in keys):
                # Use the immediately preceding t column if available.
                t_idx = j - 1 if j > 0 and cleaned[j - 1] == "t" else 0
                found[var] = (t_idx, j)
                break
    return found


def normalize_file(src: Path, dst: Path, strict: bool = False) -> None:
    if not src.exists():
        print(f"[warn] missing input: {src}", file=sys.stderr)
        return

    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open("r", newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            raise ValueError(f"empty file: {src}")
        idx = find_var_indices(header)
        missing = [v for v in VARIABLES if v not in idx]
        if missing:
            msg = f"{src}: missing variables in header: {missing}. Available header: {header}"
            if strict:
                raise KeyError(msg)
            print(f"[warn] {msg}", file=sys.stderr)

        # Prefer the time associated with inlet stage, otherwise first detected variable.
        time_source = idx.get("inlet_stage") or next(iter(idx.values()), (0, 0))
        default_time_idx = time_source[0]

        with dst.open("w", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            writer.writerow(OUT_HEADER)
            for row in reader:
                if not row:
                    continue
                out_row = [row[default_time_idx] if default_time_idx < len(row) else "nan"]
                for var in VARIABLES:
                    if var in idx:
                        _, value_idx = idx[var]
                        out_row.append(row[value_idx] if value_idx < len(row) else "nan")
                    else:
                        out_row.append("nan")
                writer.writerow(out_row)
    print(f"[ok] wrote {dst}", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--truth", type=Path, default=Path("Wetland_truth/outputs/selected_output.csv"))
    ap.add_argument("--assim", type=Path, default=Path("Wetland_assimilation/outputs/selected_output.csv"))
    ap.add_argument("--reanalysis", type=Path, default=Path("Wetland_assimilation/outputs/reanalysis_output.csv"))
    ap.add_argument("--outdir", type=Path, default=Path("Wetland_assimilation/outputs/paper_plot_inputs"))
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    normalize_file(args.truth, args.outdir / "truth_normalized.csv", args.strict)
    normalize_file(args.assim, args.outdir / "assim_normalized.csv", args.strict)
    normalize_file(args.reanalysis, args.outdir / "reanalysis_normalized.csv", args.strict)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
