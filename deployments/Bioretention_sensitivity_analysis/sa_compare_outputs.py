#!/usr/bin/env python3
"""
Sensitivity-analysis comparison for OpenHydroQual / OHTwin observedoutput.txt folders.

Expected folder layout:
    SA_ROOT/
      1_Deterministic/observedoutput.txt
      1_Deterministic/fit_measures.txt
      2_RC_L/observedoutput.txt
      2_RC_L/fit_measures.txt
      3_RC_U/observedoutput.txt
      3_RC_U/fit_measures.txt
      ...

Observed output format:
    t, Output1, t, Output2, t, Output3, ...
    time1,value1,time2,value2,...

Fit-measures format:
    MSE, R2, NSE, MSE, R2, NSE, ...
where each group of 3 belongs to one observed output in the same order
as observedoutput.txt.

Main outputs:
    SA_Results/summary_metrics.csv
    SA_Results/sensitivity_vs_deterministic.csv
    SA_Results/rank_sensitivity.csv
    SA_Results/fit_measures_named.csv
    SA_Results/fit_measures_selected.csv
    SA_Results/combined_timeseries_<output>.csv
    SA_Results/plot_<output>.gp
    SA_Results/<output>.png
    SA_Results/tornado_<metric>.csv
    SA_Results/tornado_<metric>_<output>.dat
    SA_Results/plot_tornado_<metric>_<output>.gp
    SA_Results/tornado_<metric>_<output>.png

Example:
    python3 sa_compare_outputs.py --root .
    python3 sa_compare_outputs.py --root . --tornado-metric rel_delta_auc
    python3 sa_compare_outputs.py --root . --tornado-metrics max_abs_diff_vs_det rel_delta_max rel_delta_min
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


DEFAULT_OUTPUTS = [
    "Pond water depth (m)",
    "Soil Moisture",
    "Underdrain flow (m3/day)",
]


@dataclass
class Series:
    time: List[float]
    value: List[float]


@dataclass
class RunData:
    folder: str
    order: int
    tag: str
    param: str
    side: str
    series: Dict[str, Series]
    output_order: List[str]
    fit: Dict[str, Dict[str, float]]


def safe_float(x: str) -> float:
    x = x.strip()
    if x == "":
        return float("nan")
    try:
        return float(x)
    except ValueError:
        return float("nan")


def clean_name_for_file(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", name.strip())
    s = re.sub(r"_+", "_", s).strip("_")
    return s or "output"


def parse_folder_name(folder_name: str) -> Tuple[int, str, str, str]:
    """
    Parses names like:
      1_Deterministic
      2_RC_L
      3_RC_U
      10_EngineeredSoilKsat_L
    """
    m = re.match(r"^(\d+)[_\-\s]+(.+)$", folder_name)
    if m:
        order = int(m.group(1))
        tag = m.group(2)
    else:
        order = 999999
        tag = folder_name

    parts = tag.split("_")
    side = ""
    if parts and parts[-1].upper() in {"L", "LOW", "U", "UP", "HIGH", "H"}:
        side = parts[-1].upper()
        param = "_".join(parts[:-1]) if len(parts) > 1 else tag
    else:
        param = tag

    return order, tag, param, side


def side_label(side: str) -> str:
    s = side.upper()
    if s in {"L", "LOW"}:
        return "Low"
    if s in {"U", "UP", "HIGH", "H"}:
        return "High"
    return side or "Case"


def read_observedoutput(path: Path) -> Tuple[Dict[str, Series], List[str]]:
    with path.open("r", encoding="utf-8", errors="replace", newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            raise ValueError(f"Empty file: {path}")

        header = [h.strip() for h in header]
        if len(header) < 2:
            raise ValueError(f"Not enough columns in {path}")

        value_cols = list(range(1, len(header), 2))
        names = [header[i] for i in value_cols]
        out: Dict[str, Series] = {name: Series(time=[], value=[]) for name in names}

        for row in reader:
            if not row:
                continue
            if len(row) < len(header):
                row = row + [""] * (len(header) - len(row))

            for value_col, name in zip(value_cols, names):
                time_col = value_col - 1
                t = safe_float(row[time_col])
                v = safe_float(row[value_col])
                if math.isfinite(t) and math.isfinite(v):
                    out[name].time.append(t)
                    out[name].value.append(v)

    return out, names


def read_fit_measures(path: Path, output_names: List[str]) -> Dict[str, Dict[str, float]]:
    if not path.exists():
        return {}

    text = path.read_text(encoding="utf-8", errors="replace")
    parts = [p.strip() for p in text.replace("\n", ",").split(",") if p.strip() != ""]
    vals = [safe_float(p) for p in parts]
    metrics = ["MSE", "R2", "NSE"]

    fit: Dict[str, Dict[str, float]] = {}
    for i, outname in enumerate(output_names):
        base = 3 * i
        if base + 2 >= len(vals):
            break
        fit[outname] = {
            metrics[0]: vals[base],
            metrics[1]: vals[base + 1],
            metrics[2]: vals[base + 2],
        }
    return fit


def trapz(time: List[float], value: List[float]) -> float:
    if len(time) < 2:
        return 0.0
    total = 0.0
    for i in range(1, len(time)):
        dt = time[i] - time[i - 1]
        if math.isfinite(dt) and dt >= 0:
            total += 0.5 * (value[i] + value[i - 1]) * dt
    return total


def mean(values: List[float]) -> float:
    vals = [v for v in values if math.isfinite(v)]
    return sum(vals) / len(vals) if vals else float("nan")


def minmax(values: List[float]) -> Tuple[float, float]:
    vals = [v for v in values if math.isfinite(v)]
    return (min(vals), max(vals)) if vals else (float("nan"), float("nan"))


def interp_linear(x: List[float], y: List[float], xi: float) -> float:
    if not x:
        return float("nan")
    if xi <= x[0]:
        return y[0]
    if xi >= x[-1]:
        return y[-1]

    lo, hi = 0, len(x) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if x[mid] <= xi:
            lo = mid
        else:
            hi = mid

    x0, x1 = x[lo], x[hi]
    y0, y1 = y[lo], y[hi]
    if x1 == x0:
        return y0
    f = (xi - x0) / (x1 - x0)
    return y0 + f * (y1 - y0)


def paired_error(ref: Series, run: Series) -> Tuple[float, float, float]:
    if not ref.time or not run.time:
        return float("nan"), float("nan"), float("nan")

    diffs = []
    for t, rv in zip(ref.time, ref.value):
        y = interp_linear(run.time, run.value, t)
        if math.isfinite(y) and math.isfinite(rv):
            diffs.append(y - rv)

    if not diffs:
        return float("nan"), float("nan"), float("nan")

    rmse = math.sqrt(sum(d * d for d in diffs) / len(diffs))
    mae = sum(abs(d) for d in diffs) / len(diffs)
    max_abs = max(abs(d) for d in diffs)
    return rmse, mae, max_abs


def rel_change(value: float, ref: float) -> float:
    if not math.isfinite(value) or not math.isfinite(ref):
        return float("nan")
    denom = abs(ref)
    if denom < 1e-30:
        return float("nan")
    return (value - ref) / denom


def load_runs(root: Path, observed_name: str, fit_name: str) -> List[RunData]:
    runs: List[RunData] = []

    for p in sorted(root.iterdir()):
        if not p.is_dir():
            continue
        obs = p / observed_name
        if not obs.exists():
            continue

        order, tag, param, side = parse_folder_name(p.name)
        try:
            series, output_order = read_observedoutput(obs)
            fit = read_fit_measures(p / fit_name, output_order)
        except Exception as exc:
            print(f"[WARN] Skipping {obs}: {exc}")
            continue

        runs.append(RunData(
            folder=p.name,
            order=order,
            tag=tag,
            param=param,
            side=side,
            series=series,
            output_order=output_order,
            fit=fit,
        ))

    runs.sort(key=lambda r: (r.order, r.folder))
    return runs


def write_fit_tables(runs: List[RunData], outputs: List[str], outdir: Path) -> None:
    with (outdir / "fit_measures_named.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["folder", "order", "tag", "parameter", "side", "output", "MSE", "R2", "NSE"])
        for run in runs:
            for outname in run.output_order:
                m = run.fit.get(outname)
                if not m:
                    continue
                w.writerow([
                    run.folder, run.order, run.tag, run.param, run.side, outname,
                    m.get("MSE", ""), m.get("R2", ""), m.get("NSE", ""),
                ])

    with (outdir / "fit_measures_selected.csv").open("w", newline="", encoding="utf-8") as f:
        header = ["folder", "order", "tag", "parameter", "side"]
        for outname in outputs:
            short = clean_name_for_file(outname)
            header += [f"{short}_MSE", f"{short}_R2", f"{short}_NSE"]
        w = csv.writer(f)
        w.writerow(header)

        for run in runs:
            row = [run.folder, run.order, run.tag, run.param, run.side]
            for outname in outputs:
                m = run.fit.get(outname, {})
                row += [m.get("MSE", ""), m.get("R2", ""), m.get("NSE", "")]
            w.writerow(row)


def write_summary(
    runs: List[RunData],
    outputs: List[str],
    outdir: Path,
    deterministic: RunData,
) -> None:
    summary_path = outdir / "summary_metrics.csv"
    sensitivity_path = outdir / "sensitivity_vs_deterministic.csv"

    with summary_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "folder", "order", "tag", "parameter", "side", "output",
            "n", "t_start", "t_end",
            "min", "max", "mean", "final", "auc",
            "fit_MSE", "fit_R2", "fit_NSE",
        ])

        for run in runs:
            for outname in outputs:
                s = run.series.get(outname)
                if s is None:
                    continue
                mn, mx = minmax(s.value)
                fit = run.fit.get(outname, {})
                w.writerow([
                    run.folder, run.order, run.tag, run.param, run.side, outname,
                    len(s.value),
                    s.time[0] if s.time else "",
                    s.time[-1] if s.time else "",
                    mn, mx, mean(s.value),
                    s.value[-1] if s.value else "",
                    trapz(s.time, s.value),
                    fit.get("MSE", ""),
                    fit.get("R2", ""),
                    fit.get("NSE", ""),
                ])

    det_metrics = {}
    for outname in outputs:
        s = deterministic.series.get(outname)
        if s is None:
            continue
        mn, mx = minmax(s.value)
        det_metrics[outname] = {
            "min": mn,
            "max": mx,
            "mean": mean(s.value),
            "final": s.value[-1] if s.value else float("nan"),
            "auc": trapz(s.time, s.value),
            "series": s,
        }

    with sensitivity_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "folder", "order", "tag", "parameter", "side", "output",
            "delta_min", "rel_delta_min",
            "delta_max", "rel_delta_max",
            "delta_mean", "rel_delta_mean",
            "delta_final", "rel_delta_final",
            "delta_auc", "rel_delta_auc",
            "rmse_vs_det", "mae_vs_det", "max_abs_diff_vs_det",
            "fit_MSE", "fit_R2", "fit_NSE",
        ])

        for run in runs:
            for outname in outputs:
                s = run.series.get(outname)
                d = det_metrics.get(outname)
                if s is None or d is None:
                    continue

                mn, mx = minmax(s.value)
                mu = mean(s.value)
                fin = s.value[-1] if s.value else float("nan")
                area = trapz(s.time, s.value)
                rmse, mae, max_abs = paired_error(d["series"], s)
                fit = run.fit.get(outname, {})

                w.writerow([
                    run.folder, run.order, run.tag, run.param, run.side, outname,
                    mn - d["min"], rel_change(mn, d["min"]),
                    mx - d["max"], rel_change(mx, d["max"]),
                    mu - d["mean"], rel_change(mu, d["mean"]),
                    fin - d["final"], rel_change(fin, d["final"]),
                    area - d["auc"], rel_change(area, d["auc"]),
                    rmse, mae, max_abs,
                    fit.get("MSE", ""), fit.get("R2", ""), fit.get("NSE", ""),
                ])


def write_combined_timeseries(
    runs: List[RunData],
    outputs: List[str],
    outdir: Path,
    deterministic: RunData,
) -> Dict[str, Path]:
    paths = {}

    for outname in outputs:
        ref = deterministic.series.get(outname)
        if ref is None:
            continue

        output_file = outdir / f"combined_timeseries_{clean_name_for_file(outname)}.csv"
        with output_file.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            valid_runs = [r for r in runs if outname in r.series]
            w.writerow(["time"] + [r.folder for r in valid_runs])
            for t in ref.time:
                row = [t]
                for r in valid_runs:
                    s = r.series[outname]
                    row.append(interp_linear(s.time, s.value, t))
                w.writerow(row)

        paths[outname] = output_file

    return paths


def write_rank_table(sensitivity_csv: Path, outdir: Path) -> None:
    rows = []
    with sensitivity_csv.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                score = (
                    abs(float(r.get("rel_delta_min", "nan"))) +
                    abs(float(r["rel_delta_max"])) +
                    abs(float(r["rel_delta_auc"])) +
                    abs(float(r["rel_delta_mean"]))
                )
            except Exception:
                score = float("nan")
            if math.isfinite(score):
                r["combined_abs_rel_score"] = score
                rows.append(r)

    rows.sort(key=lambda r: float(r["combined_abs_rel_score"]), reverse=True)

    with (outdir / "rank_sensitivity.csv").open("w", newline="", encoding="utf-8") as f:
        fieldnames = [
            "rank", "folder", "parameter", "side", "output",
            "combined_abs_rel_score",
            "rel_delta_min", "rel_delta_max", "rel_delta_mean", "rel_delta_auc",
            "rmse_vs_det", "mae_vs_det", "fit_MSE", "fit_R2", "fit_NSE",
        ]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for i, r in enumerate(rows, start=1):
            w.writerow({
                "rank": i,
                "folder": r["folder"],
                "parameter": r["parameter"],
                "side": r["side"],
                "output": r["output"],
                "combined_abs_rel_score": r["combined_abs_rel_score"],
                "rel_delta_min": r.get("rel_delta_min", ""),
                "rel_delta_max": r["rel_delta_max"],
                "rel_delta_mean": r["rel_delta_mean"],
                "rel_delta_auc": r["rel_delta_auc"],
                "rmse_vs_det": r["rmse_vs_det"],
                "mae_vs_det": r["mae_vs_det"],
                "fit_MSE": r.get("fit_MSE", ""),
                "fit_R2": r.get("fit_R2", ""),
                "fit_NSE": r.get("fit_NSE", ""),
            })


def write_gnuplot_scripts(
    runs: List[RunData],
    outputs: List[str],
    combined_paths: Dict[str, Path],
    outdir: Path,
    make_png: bool = True,
) -> None:
    gnuplot = shutil.which("gnuplot")

    for outname in outputs:
        data_path = combined_paths.get(outname)
        if data_path is None:
            continue

        plot_name = clean_name_for_file(outname)
        gp_path = outdir / f"plot_{plot_name}.gp"
        png_path = outdir / f"{plot_name}.png"

        valid_runs = [r for r in runs if outname in r.series]
        plot_lines = []
        for i, r in enumerate(valid_runs, start=2):
            lw = 3 if "deterministic" in r.folder.lower() else 1.5
            dt = 1 if "deterministic" in r.folder.lower() else (2 + (i % 6))
            title = r.folder.replace("_", " ")
            plot_lines.append(
                f'    "{data_path.name}" using 1:{i} with lines lw {lw} dashtype {dt} title "{title}"'
            )

        plot_body = ", \\\n".join(plot_lines)
        script = (
            'set datafile separator comma\n'
            'set terminal pngcairo size 1400,850 enhanced font "Arial,18"\n'
            f'set output "{png_path.name}"\n'
            'set grid\n'
            'set key outside right top\n'
            'set xlabel "Time"\n'
            f'set ylabel "{outname}"\n'
            f'set title "{outname} sensitivity comparison"\n'
            'plot \\\n'
            f'{plot_body}\n'
        )
        gp_path.write_text(script, encoding="utf-8")

        if make_png and gnuplot:
            try:
                subprocess.run([gnuplot, gp_path.name], cwd=outdir, check=True)
                print(f"[INFO] Wrote PNG: {png_path.name}")
            except subprocess.CalledProcessError as exc:
                print(f"[WARN] gnuplot failed for {gp_path}: {exc}")


def write_tornado_tables_and_plots(
    runs: List[RunData],
    outputs: List[str],
    outdir: Path,
    metric: str = "rel_delta_max",
    make_png: bool = True,
) -> None:
    """
    Make paper-style tornado diagrams from *_L and *_U sensitivity folders.

    Geometry:
      * Lower-bound change is always drawn to the LEFT of the zero line.
      * Upper-bound change is always drawn to the RIGHT of the zero line.
      * Bars are sorted by swing = abs(upper - lower), matching the paper's
        tornado-diagram definition.
      * Gnuplot is the primary plotting engine; .dat and .gp are always written.
    """
    sens_path = outdir / "sensitivity_vs_deterministic.csv"
    if not sens_path.exists():
        print("[WARN] Tornado skipped: sensitivity_vs_deterministic.csv not found.")
        return

    rows = []
    with sens_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            side = r.get("side", "").upper()
            if side not in {"L", "LOW", "U", "UP", "HIGH", "H"}:
                continue
            if r.get("output") not in outputs:
                continue
            try:
                val = float(r.get(metric, "nan"))
            except Exception:
                val = float("nan")
            if not math.isfinite(val):
                continue
            r["_metric_value"] = val
            rows.append(r)

    if not rows:
        print("[WARN] Tornado skipped: no rows with L/U side labels were found.")
        print("       Folder names must end like *_L and *_U, for example 2_RC_L and 3_RC_U.")
        return

    grouped: Dict[Tuple[str, str], Dict[str, dict]] = {}
    for r in rows:
        key = (r["output"], r["parameter"])
        grouped.setdefault(key, {})
        grouped[key][side_label(r["side"])] = r

    tornado_csv = outdir / f"tornado_{metric}.csv"
    with tornado_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "output", "parameter",
            "low_folder", "low_value",
            "high_folder", "high_value",
            "span", "abs_span",
            "dominant_direction",
        ])

        for (outname, param), d in sorted(grouped.items()):
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_metric_value"]) if low else float("nan")
            high_val = float(high["_metric_value"]) if high else float("nan")

            if math.isfinite(low_val) and math.isfinite(high_val):
                span = high_val - low_val
                dom = "High increases" if high_val > low_val else "Low increases"
            elif math.isfinite(low_val):
                span = low_val
                dom = "Low only"
            elif math.isfinite(high_val):
                span = high_val
                dom = "High only"
            else:
                continue

            w.writerow([
                outname, param,
                low["folder"] if low else "",
                low_val if math.isfinite(low_val) else "",
                high["folder"] if high else "",
                high_val if math.isfinite(high_val) else "",
                span,
                abs(span),
                dom,
            ])

    print(f"[INFO] Wrote tornado table: {tornado_csv.name}")

    gnuplot = shutil.which("gnuplot")
    if make_png and not gnuplot:
        print("[WARN] gnuplot not found. Tornado .dat/.gp files were written, but PNGs were not generated.")

    for outname in outputs:
        items = []
        for (o, param), d in grouped.items():
            if o != outname:
                continue

            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_metric_value"]) if low else float("nan")
            high_val = float(high["_metric_value"]) if high else float("nan")

            if math.isfinite(low_val) and math.isfinite(high_val):
                span_abs = abs(high_val - low_val)
            elif math.isfinite(low_val):
                span_abs = abs(low_val)
            elif math.isfinite(high_val):
                span_abs = abs(high_val)
            else:
                continue

            items.append({
                "parameter": param,
                "low": low_val,
                "high": high_val,
                "low_abs": abs(low_val) if math.isfinite(low_val) else 0.0,
                "high_abs": abs(high_val) if math.isfinite(high_val) else 0.0,
                "span_abs": span_abs,
            })

        items.sort(key=lambda x: x["span_abs"], reverse=True)
        if not items:
            print(f"[WARN] No tornado items for output: {outname}")
            continue

        slug = clean_name_for_file(outname)
        dat_path = outdir / f"tornado_{metric}_{slug}.dat"
        gp_path = outdir / f"plot_tornado_{metric}_{slug}.gp"
        png_path = outdir / f"tornado_{metric}_{slug}.png"

        with dat_path.open("w", encoding="utf-8") as f:
            f.write("# y label low high abs_span low_abs high_abs low_center low_half high_center high_half\n")
            n = len(items)
            for i, item in enumerate(items):
                y = n - i
                low_abs = item["low_abs"]
                high_abs = item["high_abs"]

                low_half = max(low_abs * 0.5, 1e-14)
                high_half = max(high_abs * 0.5, 1e-14)
                low_center = -low_half
                high_center = high_half

                label = item["parameter"].replace('"', "'")
                f.write(
                    f'{y} "{label}" {item["low"]:.12g} {item["high"]:.12g} '
                    f'{item["span_abs"]:.12g} {low_abs:.12g} {high_abs:.12g} '
                    f'{low_center:.12g} {low_half:.12g} {high_center:.12g} {high_half:.12g}\n'
                )

        max_abs = max(
            [item["low_abs"] for item in items] + [item["high_abs"] for item in items] + [1e-12]
        )
        xmax = max_abs * 1.12
        xmin = -xmax

        xlabel = {
            "rel_delta_min": "Minimum response change",
            "rel_delta_max": "Peak response change",
            "rel_delta_mean": "Mean response change",
            "rel_delta_auc": "Integrated response change",
            "rmse_vs_det": "RMSE vs deterministic",
            "max_abs_diff_vs_det": "Maximum absolute difference vs deterministic",
        }.get(metric, metric)

        gp = (
            'set terminal pngcairo size 1500,900 enhanced font "Arial,18"\n'
            f'set output "{png_path.name}"\n'
            'set datafile separator whitespace\n'
            'set style fill solid 0.82 border rgb "black"\n'
            'unset key\n'
            'unset title\n'
            'set grid x\n'
            'set style line 1 lc rgb "#BDBDBD"\n'
            'set style line 2 lc rgb "#F28E2B"\n'
            f'set xlabel "{xlabel}"\n'
            'set ylabel "Parameters"\n'
            f'set xrange [{xmin:.12g}:{xmax:.12g}]\n'
            f'set yrange [0:{len(items)+1}]\n'
            'set xzeroaxis lw 2 lc rgb "black"\n'
            'set tics out\n'
            'set border lw 1.2\n'
            'set format x "%g"\n'
            f'plot "{dat_path.name}" using 8:1:9:(0.33):ytic(2) with boxxyerrorbars ls 1, \\\n'
            f'     "{dat_path.name}" using 10:1:11:(0.33) with boxxyerrorbars ls 2\n'
        )
        gp_path.write_text(gp, encoding="utf-8")
        print(f"[INFO] Wrote tornado data: {dat_path.name}")
        print(f"[INFO] Wrote tornado gnuplot: {gp_path.name}")

        if make_png and gnuplot:
            if png_path.exists():
                try:
                    png_path.unlink()
                except OSError:
                    pass
            try:
                subprocess.run([gnuplot, gp_path.name], cwd=outdir, check=True)
                if png_path.exists() and png_path.stat().st_size > 0:
                    print(f"[INFO] Wrote tornado PNG by gnuplot: {png_path.name} ({png_path.stat().st_size} bytes)")
                else:
                    print(f"[WARN] Tornado PNG is empty or missing after gnuplot: {png_path.name}")
            except subprocess.CalledProcessError as exc:
                print(f"[WARN] gnuplot failed for tornado {gp_path}: {exc}")


def normalize_param_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", name.lower())


def parse_key_value_fields(line: str) -> Dict[str, str]:
    parts = [x.strip() for x in line.strip().split(';', 1)[-1].split(',')]
    out: Dict[str, str] = {}
    for part in parts:
        if '=' not in part:
            continue
        k, v = part.split('=', 1)
        out[k.strip()] = v.strip()
    return out


def read_ohq_parameter_values(path: Path) -> Dict[str, float]:
    vals: Dict[str, float] = {}
    if not path.exists():
        return vals
    text = path.read_text(encoding='utf-8', errors='replace')
    for raw in text.splitlines():
        line = raw.strip()
        if not line.lower().startswith('create parameter'):
            continue
        fields = parse_key_value_fields(line)
        name = fields.get('name', '')
        value = fields.get('value', '')
        if not name:
            continue
        v = safe_float(value)
        if math.isfinite(v):
            vals[normalize_param_name(name)] = v
    return vals


def find_ohq_file(run_dir: Path, preferred_name: str = "") -> Path | None:
    if preferred_name:
        p = run_dir / preferred_name
        if p.exists():
            return p
    files = sorted(run_dir.glob('*.ohq')) or sorted(run_dir.glob('*.OHQ'))
    return files[0] if files else None


def pretty_parameter_label(param: str) -> str:
    aliases = {
        "RunoffCoeff": "Runoff coeff.",
        "Manning'sRoughness": "Manning roughness",
        "EngineeredSoilKsat": "Engineered soil Ksat",
        "EngineeredSoilAlpha": "Engineered soil α",
        "EngineeredSoiln": "Engineered soil n",
        "NativeSoilKsat": "Native soil Ksat",
        "NativeSoilAlpha": "Native soil α",
        "NativeSoiln": "Native soil n",
        "EvapCoeff": "Evap. coeff.",
    }
    if param in aliases:
        return aliases[param]
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", param).replace("_", " ")



def regression_metrics_against_reference(ref: Series, run: Series) -> Dict[str, float]:
    """
    Calculate deterministic-time-series-based fit metrics.

    The deterministic selected output is treated as the observed/reference
    series. The sensitivity run is interpolated to the deterministic time grid.
    Returned R2 is the squared Pearson correlation coefficient. NSE is the
    Nash-Sutcliffe efficiency, 1 - SSE/SST, using the deterministic series as
    the reference observations.
    """
    pairs: List[Tuple[float, float]] = []
    if not ref.time or not run.time:
        return {
            "n_pairs": 0,
            "MSE": float("nan"),
            "RMSE": float("nan"),
            "MAE": float("nan"),
            "R2": float("nan"),
            "NSE": float("nan"),
            "max_abs_diff": float("nan"),
        }

    for t, y_ref in zip(ref.time, ref.value):
        y_run = interp_linear(run.time, run.value, t)
        if math.isfinite(y_ref) and math.isfinite(y_run):
            pairs.append((y_ref, y_run))

    n = len(pairs)
    if n == 0:
        return {
            "n_pairs": 0,
            "MSE": float("nan"),
            "RMSE": float("nan"),
            "MAE": float("nan"),
            "R2": float("nan"),
            "NSE": float("nan"),
            "max_abs_diff": float("nan"),
        }

    obs = [a for a, _ in pairs]
    sim = [b for _, b in pairs]
    diffs = [b - a for a, b in pairs]
    sse = sum(d * d for d in diffs)
    mse = sse / n
    rmse = math.sqrt(mse)
    mae = sum(abs(d) for d in diffs) / n
    max_abs = max(abs(d) for d in diffs)

    obs_mean = sum(obs) / n
    sim_mean = sum(sim) / n
    sst = sum((a - obs_mean) ** 2 for a in obs)
    nse = 1.0 - sse / sst if sst > 1e-300 else float("nan")

    ss_obs = sum((a - obs_mean) ** 2 for a in obs)
    ss_sim = sum((b - sim_mean) ** 2 for b in sim)
    if ss_obs > 1e-300 and ss_sim > 1e-300:
        cov = sum((a - obs_mean) * (b - sim_mean) for a, b in pairs)
        r2 = (cov * cov) / (ss_obs * ss_sim)
    else:
        r2 = float("nan")

    return {
        "n_pairs": n,
        "MSE": mse,
        "RMSE": rmse,
        "MAE": mae,
        "R2": r2,
        "NSE": nse,
        "max_abs_diff": max_abs,
    }


def write_calculated_fit_tables(
    runs: List[RunData],
    outputs: List[str],
    outdir: Path,
    deterministic: RunData,
) -> None:
    """
    Calculate MSE/R2/NSE directly from observedoutput.txt time series.

    For each selected output, the deterministic run is treated as the reference
    observed time series. Each run is interpolated to the deterministic time
    grid before metrics are calculated.
    """
    long_path = outdir / "calculated_metrics_vs_deterministic.csv"
    selected_path = outdir / "calculated_metrics_selected.csv"
    metrics_by_run: Dict[Tuple[str, str], Dict[str, float]] = {}

    with long_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "folder", "order", "tag", "parameter", "side", "output",
            "n_pairs", "calc_MSE", "calc_RMSE", "calc_MAE", "calc_R2", "calc_NSE", "calc_max_abs_diff",
        ])
        for run in runs:
            for outname in outputs:
                ref = deterministic.series.get(outname)
                s = run.series.get(outname)
                if ref is None or s is None:
                    continue
                m = regression_metrics_against_reference(ref, s)
                metrics_by_run[(run.folder, outname)] = m
                w.writerow([
                    run.folder, run.order, run.tag, run.param, run.side, outname,
                    m["n_pairs"], m["MSE"], m["RMSE"], m["MAE"], m["R2"], m["NSE"], m["max_abs_diff"],
                ])
    print(f"[INFO] Wrote calculated deterministic-reference metrics: {long_path.name}")

    with selected_path.open("w", newline="", encoding="utf-8") as f:
        header = ["folder", "order", "tag", "parameter", "side"]
        for outname in outputs:
            short = clean_name_for_file(outname)
            header += [f"{short}_calc_MSE", f"{short}_calc_R2", f"{short}_calc_NSE"]
        w = csv.writer(f)
        w.writerow(header)
        for run in runs:
            row = [run.folder, run.order, run.tag, run.param, run.side]
            for outname in outputs:
                m = metrics_by_run.get((run.folder, outname), {})
                row += [m.get("MSE", ""), m.get("R2", ""), m.get("NSE", "")]
            w.writerow(row)
    print(f"[INFO] Wrote selected calculated metrics: {selected_path.name}")


def write_calculated_mse_tornado_plots(
    outputs: List[str],
    outdir: Path,
    make_png: bool = True,
) -> None:
    """
    Make additional tornado-style plots using MSE calculated directly from
    deterministic observedoutput.txt time series.

    These are intentionally separate from:
      * tornado_<metric>_* files from time-series response metrics; and
      * tornado_mse_logsens_* files from fit_measures.txt + parameter log-sensitivity.
    """
    calc_path = outdir / "calculated_metrics_vs_deterministic.csv"
    if not calc_path.exists():
        print("[WARN] Calculated-MSE tornado skipped: calculated_metrics_vs_deterministic.csv not found.")
        return

    rows = []
    with calc_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            side = r.get("side", "").upper()
            if side not in {"L", "LOW", "U", "UP", "HIGH", "H"}:
                continue
            if r.get("output") not in outputs:
                continue
            try:
                val = float(r.get("calc_MSE", "nan"))
            except Exception:
                val = float("nan")
            if not math.isfinite(val):
                continue
            r["_mse"] = val
            rows.append(r)

    summary_path = outdir / "tornado_calc_mse.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "output", "parameter", "low_folder", "low_calc_MSE", "high_folder", "high_calc_MSE",
            "span", "abs_span", "dominant_direction",
        ])

        grouped: Dict[Tuple[str, str], Dict[str, dict]] = {}
        for r in rows:
            grouped.setdefault((r["output"], r["parameter"]), {})[side_label(r["side"])] = r

        for (outname, param), d in sorted(grouped.items()):
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_mse"]) if low else float("nan")
            high_val = float(high["_mse"]) if high else float("nan")
            if math.isfinite(low_val) and math.isfinite(high_val):
                span = high_val - low_val
                dom = "High larger MSE" if high_val > low_val else "Low larger MSE"
            elif math.isfinite(low_val):
                span = low_val
                dom = "Low only"
            elif math.isfinite(high_val):
                span = high_val
                dom = "High only"
            else:
                continue
            w.writerow([
                outname, param,
                low["folder"] if low else "", low_val if math.isfinite(low_val) else "",
                high["folder"] if high else "", high_val if math.isfinite(high_val) else "",
                span, abs(span), dom,
            ])
    print(f"[INFO] Wrote calculated-MSE tornado table: {summary_path.name}")

    if not rows:
        print("[WARN] Calculated-MSE tornado produced no rows with L/U side labels.")
        return

    grouped: Dict[Tuple[str, str], Dict[str, dict]] = {}
    for r in rows:
        grouped.setdefault((r["output"], r["parameter"]), {})[side_label(r["side"])] = r

    gnuplot = shutil.which("gnuplot")
    if make_png and not gnuplot:
        print("[WARN] gnuplot not found. Calculated-MSE tornado .dat/.gp files were written, but PNGs were not generated.")

    for outname in outputs:
        items = []
        for (o, param), d in grouped.items():
            if o != outname:
                continue
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_mse"]) if low else float("nan")
            high_val = float(high["_mse"]) if high else float("nan")
            vals = [v for v in (low_val, high_val) if math.isfinite(v)]
            if not vals:
                continue
            span_abs = abs(high_val - low_val) if math.isfinite(low_val) and math.isfinite(high_val) else max(abs(v) for v in vals)
            items.append({
                "parameter": param,
                "label": pretty_parameter_label(param),
                "low": low_val,
                "high": high_val,
                "low_abs": abs(low_val) if math.isfinite(low_val) else 0.0,
                "high_abs": abs(high_val) if math.isfinite(high_val) else 0.0,
                "span_abs": span_abs,
            })

        items.sort(key=lambda x: x["span_abs"], reverse=True)
        if not items:
            print(f"[WARN] No calculated-MSE tornado items for output: {outname}")
            continue

        slug = clean_name_for_file(outname)
        dat_path = outdir / f"tornado_calc_mse_{slug}.dat"
        gp_path = outdir / f"plot_tornado_calc_mse_{slug}.gp"
        png_path = outdir / f"tornado_calc_mse_{slug}.png"

        with dat_path.open("w", encoding="utf-8") as f:
            f.write("# y label low_calc_MSE high_calc_MSE abs_span low_center low_half high_center high_half\n")
            n = len(items)
            for i, item in enumerate(items):
                y = n - i
                low_half = max(item["low_abs"] * 0.5, 1e-14)
                high_half = max(item["high_abs"] * 0.5, 1e-14)
                low_center = -low_half
                high_center = high_half
                label = item["label"].replace('"', "'")
                f.write(
                    f'{y} "{label}" {item["low"]:.12g} {item["high"]:.12g} {item["span_abs"]:.12g} '
                    f'{low_center:.12g} {low_half:.12g} {high_center:.12g} {high_half:.12g}\n'
                )

        max_abs = max([item["low_abs"] for item in items] + [item["high_abs"] for item in items] + [1e-12])
        xmax = max_abs * 1.12
        xmin = -xmax
        gp = (
            'set terminal pngcairo size 1500,900 enhanced font "Arial,18"\n'
            f'set output "{png_path.name}"\n'
            'set datafile separator whitespace\n'
            'set style fill solid 0.82 border rgb "black"\n'
            'unset key\n'
            'unset title\n'
            'set grid x\n'
            'set style line 1 lc rgb "#BDBDBD"\n'
            'set style line 2 lc rgb "#F28E2B"\n'
            'set xlabel "Calculated MSE vs deterministic time series"\n'
            'set ylabel "Parameters"\n'
            f'set xrange [{xmin:.12g}:{xmax:.12g}]\n'
            f'set yrange [0:{len(items)+1}]\n'
            'set xzeroaxis lw 2 lc rgb "black"\n'
            'set tics out\n'
            'set border lw 1.2\n'
            'set format x "%g"\n'
            f'plot "{dat_path.name}" using 6:1:7:(0.33):ytic(2) with boxxyerrorbars ls 1, \\\n'
            f'     "{dat_path.name}" using 8:1:9:(0.33) with boxxyerrorbars ls 2\n'
        )
        gp_path.write_text(gp, encoding="utf-8")
        print(f"[INFO] Wrote calculated-MSE tornado data: {dat_path.name}")
        print(f"[INFO] Wrote calculated-MSE tornado gnuplot: {gp_path.name}")

        if make_png and gnuplot:
            if png_path.exists():
                try:
                    png_path.unlink()
                except OSError:
                    pass
            try:
                subprocess.run([gnuplot, gp_path.name], cwd=outdir, check=True)
                if png_path.exists() and png_path.stat().st_size > 0:
                    print(f"[INFO] Wrote calculated-MSE tornado PNG by gnuplot: {png_path.name} ({png_path.stat().st_size} bytes)")
                else:
                    print(f"[WARN] Calculated-MSE tornado PNG is empty or missing after gnuplot: {png_path.name}")
            except subprocess.CalledProcessError as exc:
                print(f"[WARN] gnuplot failed for calculated-MSE tornado {gp_path}: {exc}")

def write_mse_log_sensitivity_plots(
    root: Path,
    runs: List[RunData],
    outputs: List[str],
    outdir: Path,
    deterministic: RunData,
    ohq_name: str = "",
    make_png: bool = True,
) -> None:
    """
    Separate non-overwriting plots for d(ln(MSE))/d(ln(p)).
    Files use tornado_mse_logsens_* and plot_tornado_mse_logsens_* prefixes only.
    Existing tornado_* files are not touched.
    """
    gnuplot = shutil.which("gnuplot")
    det_ohq = find_ohq_file(root / deterministic.folder, ohq_name)
    if det_ohq is None:
        print(f"[WARN] MSE log-sensitivity skipped: no .ohq file found in {root / deterministic.folder}")
        return
    det_params = read_ohq_parameter_values(det_ohq)
    if not det_params:
        print(f"[WARN] MSE log-sensitivity skipped: no parameter values found in {det_ohq}")
        return

    run_param_values: Dict[str, Dict[str, float]] = {}
    for run in runs:
        ohq = find_ohq_file(root / run.folder, ohq_name)
        if ohq is not None:
            run_param_values[run.folder] = read_ohq_parameter_values(ohq)

    rows = []
    for run in runs:
        side = side_label(run.side)
        if side not in {"Low", "High"}:
            continue
        pkey = normalize_param_name(run.param)
        p0 = det_params.get(pkey, float("nan"))
        pi = run_param_values.get(run.folder, {}).get(pkey, float("nan"))
        if not (math.isfinite(p0) and math.isfinite(pi)):
            print(f"[WARN] MSE log-sensitivity: missing parameter value for {run.folder} / {run.param}")
            continue
        if p0 <= 0 or pi <= 0 or abs(math.log(pi) - math.log(p0)) < 1e-30:
            print(f"[WARN] MSE log-sensitivity: invalid/unchanged p for {run.folder} / {run.param}: p0={p0}, pi={pi}")
            continue
        for outname in outputs:
            mse0 = deterministic.fit.get(outname, {}).get("MSE", float("nan"))
            msei = run.fit.get(outname, {}).get("MSE", float("nan"))
            if not (math.isfinite(mse0) and math.isfinite(msei)):
                continue
            if mse0 <= 0 or msei <= 0:
                continue
            sens = (math.log(msei) - math.log(mse0)) / (math.log(pi) - math.log(p0))
            if not math.isfinite(sens):
                continue
            rows.append({
                "output": outname,
                "parameter": run.param,
                "side": side,
                "folder": run.folder,
                "p0": p0,
                "pi": pi,
                "MSE0": mse0,
                "MSEi": msei,
                "dlnMSE_dlnP": sens,
            })

    csv_path = outdir / "tornado_mse_logsens.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        fieldnames = ["output", "parameter", "side", "folder", "p0", "pi", "MSE0", "MSEi", "dlnMSE_dlnP"]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)
    print(f"[INFO] Wrote MSE log-sensitivity table: {csv_path.name}")

    if not rows:
        print("[WARN] MSE log-sensitivity produced no rows. ln(MSE) requires MSE > 0 in fit_measures.txt.")
        return

    grouped: Dict[Tuple[str, str], Dict[str, dict]] = {}
    for r in rows:
        grouped.setdefault((r["output"], r["parameter"]), {})[r["side"]] = r

    for outname in outputs:
        items = []
        for (o, param), d in grouped.items():
            if o != outname:
                continue
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["dlnMSE_dlnP"]) if low else float("nan")
            high_val = float(high["dlnMSE_dlnP"]) if high else float("nan")
            vals = [v for v in (low_val, high_val) if math.isfinite(v)]
            if not vals:
                continue
            span_abs = abs(high_val - low_val) if math.isfinite(low_val) and math.isfinite(high_val) else max(abs(v) for v in vals)
            items.append({"parameter": param, "label": pretty_parameter_label(param), "low": low_val, "high": high_val, "span_abs": span_abs})
        items.sort(key=lambda x: x["span_abs"], reverse=True)
        if not items:
            print(f"[WARN] No MSE log-sensitivity items for output: {outname}")
            continue

        slug = clean_name_for_file(outname)
        dat_path = outdir / f"tornado_mse_logsens_{slug}.dat"
        gp_path = outdir / f"plot_tornado_mse_logsens_{slug}.gp"
        png_path = outdir / f"tornado_mse_logsens_{slug}.png"
        with dat_path.open("w", encoding="utf-8") as f:
            f.write("# y label low high abs_span low_center low_half high_center high_half\n")
            n = len(items)
            for i, item in enumerate(items):
                y = n - i
                low_abs = abs(item["low"]) if math.isfinite(item["low"]) else 0.0
                high_abs = abs(item["high"]) if math.isfinite(item["high"]) else 0.0
                low_half = max(low_abs * 0.5, 1e-14)
                high_half = max(high_abs * 0.5, 1e-14)
                low_center = -low_half
                high_center = high_half
                label = item["label"].replace('"', "'")
                f.write(f'{y} "{label}" {item["low"]:.12g} {item["high"]:.12g} {item["span_abs"]:.12g} {low_center:.12g} {low_half:.12g} {high_center:.12g} {high_half:.12g}\n')

        max_abs = max([abs(item["low"]) for item in items if math.isfinite(item["low"])] + [abs(item["high"]) for item in items if math.isfinite(item["high"])] + [1e-12])
        xmax = max_abs * 1.12
        xmin = -xmax
        gp = (
            'set terminal pngcairo size 1500,900 enhanced font "Arial,18"\n'
            f'set output "{png_path.name}"\n'
            'set datafile separator whitespace\n'
            'set style fill solid 0.82 border rgb "black"\n'
            'unset key\n'
            'unset title\n'
            'set grid x\n'
            'set style line 1 lc rgb "#BDBDBD"\n'
            'set style line 2 lc rgb "#F28E2B"\n'
            'set xlabel "d(ln(MSE)) / d(ln(p))"\n'
            'set ylabel "Parameters"\n'
            f'set xrange [{xmin:.12g}:{xmax:.12g}]\n'
            f'set yrange [0:{len(items)+1}]\n'
            'set xzeroaxis lw 2 lc rgb "black"\n'
            'set tics out\n'
            'set border lw 1.2\n'
            'set format x "%g"\n'
            f'plot "{dat_path.name}" using 6:1:7:(0.33):ytic(2) with boxxyerrorbars ls 1, \\\n'
            f'     "{dat_path.name}" using 8:1:9:(0.33) with boxxyerrorbars ls 2\n'
        )
        gp_path.write_text(gp, encoding="utf-8")
        print(f"[INFO] Wrote MSE log-sensitivity data: {dat_path.name}")
        print(f"[INFO] Wrote MSE log-sensitivity gnuplot: {gp_path.name}")
        if make_png and gnuplot:
            if png_path.exists():
                try:
                    png_path.unlink()
                except OSError:
                    pass
            try:
                subprocess.run([gnuplot, gp_path.name], cwd=outdir, check=True)
                if png_path.exists() and png_path.stat().st_size > 0:
                    print(f"[INFO] Wrote MSE log-sensitivity PNG by gnuplot: {png_path.name} ({png_path.stat().st_size} bytes)")
                else:
                    print(f"[WARN] MSE log-sensitivity PNG is empty or missing after gnuplot: {png_path.name}")
            except subprocess.CalledProcessError as exc:
                print(f"[WARN] gnuplot failed for MSE log-sensitivity {gp_path}: {exc}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="Root folder containing 1_Deterministic, 2_..., etc.")
    ap.add_argument("--observed-name", default="observedoutput.txt")
    ap.add_argument("--fit-name", default="fit_measures.txt")
    ap.add_argument("--deterministic", default="1_Deterministic", help="Deterministic/reference folder name.")
    ap.add_argument("--outdir", default="SA_Results")
    ap.add_argument("--outputs", nargs="+", default=DEFAULT_OUTPUTS)
    ap.add_argument("--no-gnuplot", action="store_true", help="Write CSVs and .gp files but do not run gnuplot.")
    ap.add_argument("--no-mse-log-sensitivity", action="store_true", help="Do not generate separate d(ln(MSE))/d(ln(p)) plots.")
    ap.add_argument("--ohq-name", default="", help="Optional OHQ filename inside each run folder. If omitted, the first *.ohq is used.")
    tornado_metric_choices = [
        "rel_delta_min",
        "rel_delta_max",
        "rel_delta_mean",
        "rel_delta_auc",
        "rmse_vs_det",
        "max_abs_diff_vs_det",
    ]
    ap.add_argument(
        "--tornado-metric",
        default=None,
        choices=tornado_metric_choices,
        help="Single tornado metric to generate. Kept for backward compatibility.",
    )
    ap.add_argument(
        "--tornado-metrics",
        nargs="+",
        default=None,
        choices=tornado_metric_choices + ["all"],
        help=(
            "One or more tornado metrics to generate. Use 'all' for the default set: "
            "max_abs_diff_vs_det, rel_delta_max, and rel_delta_min."
        ),
    )
    args = ap.parse_args()

    root = Path(args.root).resolve()
    outdir = root / args.outdir
    outdir.mkdir(parents=True, exist_ok=True)

    runs = load_runs(root, args.observed_name, args.fit_name)
    if not runs:
        raise SystemExit(f"No folders containing {args.observed_name} found under {root}")

    deterministic = None
    for r in runs:
        if r.folder == args.deterministic:
            deterministic = r
            break

    if deterministic is None:
        deterministic = runs[0]
        print(f"[WARN] Reference folder {args.deterministic!r} not found. Using {deterministic.folder!r}.")

    available = set()
    for r in runs:
        available.update(r.series.keys())

    outputs = []
    for o in args.outputs:
        if o in available:
            outputs.append(o)
        else:
            print(f"[WARN] Requested output not found: {o}")

    if not outputs:
        raise SystemExit("None of the requested outputs were found. Check names in observedoutput.txt header.")

    print("[INFO] Runs:")
    for r in runs:
        print(f"  {r.order:>3}  {r.folder}")

    print("[INFO] Outputs:")
    for o in outputs:
        print(f"  {o}")

    write_fit_tables(runs, outputs, outdir)
    write_summary(runs, outputs, outdir, deterministic)
    write_calculated_fit_tables(runs, outputs, outdir, deterministic)
    write_calculated_mse_tornado_plots(outputs, outdir, make_png=(not args.no_gnuplot))
    combined = write_combined_timeseries(runs, outputs, outdir, deterministic)
    write_rank_table(outdir / "sensitivity_vs_deterministic.csv", outdir)
    write_gnuplot_scripts(runs, outputs, combined, outdir, make_png=(not args.no_gnuplot))
    if args.tornado_metrics:
        tornado_metrics = args.tornado_metrics
    elif args.tornado_metric:
        tornado_metrics = [args.tornado_metric]
    else:
        tornado_metrics = ["max_abs_diff_vs_det", "rel_delta_max", "rel_delta_min"]

    if "all" in tornado_metrics:
        tornado_metrics = ["max_abs_diff_vs_det", "rel_delta_max", "rel_delta_min"]

    # Preserve order while removing duplicates.
    tornado_metrics = list(dict.fromkeys(tornado_metrics))

    for tornado_metric in tornado_metrics:
        write_tornado_tables_and_plots(
            runs, outputs, outdir, metric=tornado_metric, make_png=(not args.no_gnuplot)
        )

    if not args.no_mse_log_sensitivity:
        write_mse_log_sensitivity_plots(
            root, runs, outputs, outdir, deterministic,
            ohq_name=args.ohq_name,
            make_png=(not args.no_gnuplot),
        )

    print(f"[OK] Wrote results to: {outdir}")
    print("     summary_metrics.csv")
    print("     sensitivity_vs_deterministic.csv")
    print("     rank_sensitivity.csv")
    print("     fit_measures_named.csv")
    print("     fit_measures_selected.csv")
    print("     calculated_metrics_vs_deterministic.csv")
    print("     calculated_metrics_selected.csv")
    print("     tornado_calc_mse.csv")
    for o in outputs:
        print(f"     tornado_calc_mse_{clean_name_for_file(o)}.png")
    for tornado_metric in tornado_metrics:
        print(f"     tornado_{tornado_metric}.csv")
    if not args.no_mse_log_sensitivity:
        print("     tornado_mse_logsens.csv")
        for o in outputs:
            print(f"     tornado_mse_logsens_{clean_name_for_file(o)}.png")
    for o in outputs:
        print(f"     combined_timeseries_{clean_name_for_file(o)}.csv")
        print(f"     plot_{clean_name_for_file(o)}.gp")


if __name__ == "__main__":
    main()
