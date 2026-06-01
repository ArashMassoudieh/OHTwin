#!/usr/bin/env python3
"""
Sensitivity-analysis comparison for OpenHydroQual / OHTwin observedoutput.txt folders.

Expected folder layout:
    SA_ROOT/
      1_Deterministic/observedoutput.txt
      1_Deterministic/fit_measures.txt
      2_RunoffCoeff_L/observedoutput.txt
      2_RunoffCoeff_L/fit_measures.txt
      3_RunoffCoeff_U/observedoutput.txt
      3_RunoffCoeff_U/fit_measures.txt
      ...

Observed output format:
    t, Output1, t, Output2, t, Output3, ...
    time1,value1,time2,value2,...

Fit-measures format:
    MSE, R2, NSE, MSE, R2, NSE, ...
where each group of 3 belongs to one observed output in the same order as
observedoutput.txt.
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
    x = str(x).strip()
    if not x:
        return float("nan")
    try:
        return float(x)
    except ValueError:
        return float("nan")


def clean_name_for_file(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", name.strip())
    s = re.sub(r"_+", "_", s).strip("_")
    return s or "output"


def normalize_param_name(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", name.lower())


def parse_folder_name(folder_name: str) -> Tuple[int, str, str, str]:
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


def read_observedoutput(path: Path) -> Tuple[Dict[str, Series], List[str]]:
    with path.open("r", encoding="utf-8", errors="replace", newline="") as f:
        reader = csv.reader(f)
        try:
            header = [h.strip() for h in next(reader)]
        except StopIteration:
            raise ValueError(f"Empty file: {path}")
        if len(header) < 2:
            raise ValueError(f"Not enough columns in {path}")

        value_cols = list(range(1, len(header), 2))
        names = [header[i] for i in value_cols]
        out = {name: Series([], []) for name in names}

        for row in reader:
            if not row:
                continue
            if len(row) < len(header):
                row = row + [""] * (len(header) - len(row))
            for value_col, name in zip(value_cols, names):
                t = safe_float(row[value_col - 1])
                v = safe_float(row[value_col])
                if math.isfinite(t) and math.isfinite(v):
                    out[name].time.append(t)
                    out[name].value.append(v)
    return out, names


def read_fit_measures(path: Path, output_names: List[str]) -> Dict[str, Dict[str, float]]:
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    vals = [safe_float(p) for p in text.replace("\n", ",").split(",") if p.strip()]
    fit: Dict[str, Dict[str, float]] = {}
    for i, outname in enumerate(output_names):
        base = 3 * i
        if base + 2 >= len(vals):
            break
        fit[outname] = {"MSE": vals[base], "R2": vals[base + 1], "NSE": vals[base + 2]}
    return fit


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
        runs.append(RunData(p.name, order, tag, param, side, series, output_order, fit))
    runs.sort(key=lambda r: (r.order, r.folder))
    return runs


def mean(values: List[float]) -> float:
    vals = [v for v in values if math.isfinite(v)]
    return sum(vals) / len(vals) if vals else float("nan")


def minmax(values: List[float]) -> Tuple[float, float]:
    vals = [v for v in values if math.isfinite(v)]
    return (min(vals), max(vals)) if vals else (float("nan"), float("nan"))


def trapz(time: List[float], value: List[float]) -> float:
    if len(time) < 2:
        return 0.0
    total = 0.0
    for i in range(1, len(time)):
        dt = time[i] - time[i - 1]
        if math.isfinite(dt) and dt >= 0:
            total += 0.5 * (value[i] + value[i - 1]) * dt
    return total


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
    return y0 + (xi - x0) * (y1 - y0) / (x1 - x0)


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
    if not math.isfinite(value) or not math.isfinite(ref) or abs(ref) < 1e-30:
        return float("nan")
    return (value - ref) / abs(ref)


def regression_metrics_against_reference(ref: Series, run: Series) -> Dict[str, float]:
    pairs = []
    if not ref.time or not run.time:
        return {"n_pairs": 0, "MSE": float("nan"), "RMSE": float("nan"), "MAE": float("nan"), "R2": float("nan"), "NSE": float("nan"), "max_abs_diff": float("nan")}
    for t, y_ref in zip(ref.time, ref.value):
        y_run = interp_linear(run.time, run.value, t)
        if math.isfinite(y_ref) and math.isfinite(y_run):
            pairs.append((y_ref, y_run))
    n = len(pairs)
    if n == 0:
        return {"n_pairs": 0, "MSE": float("nan"), "RMSE": float("nan"), "MAE": float("nan"), "R2": float("nan"), "NSE": float("nan"), "max_abs_diff": float("nan")}
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
    ss_obs = sst
    ss_sim = sum((b - sim_mean) ** 2 for b in sim)
    if ss_obs > 1e-300 and ss_sim > 1e-300:
        cov = sum((a - obs_mean) * (b - sim_mean) for a, b in pairs)
        r2 = (cov * cov) / (ss_obs * ss_sim)
    else:
        r2 = float("nan")
    return {"n_pairs": n, "MSE": mse, "RMSE": rmse, "MAE": mae, "R2": r2, "NSE": nse, "max_abs_diff": max_abs}


def write_fit_tables(runs: List[RunData], outputs: List[str], outdir: Path) -> None:
    with (outdir / "fit_measures_named.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["folder", "order", "tag", "parameter", "side", "output", "MSE", "R2", "NSE"])
        for run in runs:
            for outname in run.output_order:
                m = run.fit.get(outname)
                if m:
                    w.writerow([run.folder, run.order, run.tag, run.param, run.side, outname, m.get("MSE", ""), m.get("R2", ""), m.get("NSE", "")])
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


def write_summary(runs: List[RunData], outputs: List[str], outdir: Path, deterministic: RunData) -> None:
    with (outdir / "summary_metrics.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["folder", "order", "tag", "parameter", "side", "output", "n", "t_start", "t_end", "min", "max", "mean", "final", "auc", "fit_MSE", "fit_R2", "fit_NSE"])
        for run in runs:
            for outname in outputs:
                s = run.series.get(outname)
                if s is None:
                    continue
                mn, mx = minmax(s.value)
                fit = run.fit.get(outname, {})
                w.writerow([run.folder, run.order, run.tag, run.param, run.side, outname, len(s.value), s.time[0] if s.time else "", s.time[-1] if s.time else "", mn, mx, mean(s.value), s.value[-1] if s.value else "", trapz(s.time, s.value), fit.get("MSE", ""), fit.get("R2", ""), fit.get("NSE", "")])

    det_metrics = {}
    for outname in outputs:
        s = deterministic.series.get(outname)
        if s is None:
            continue
        mn, mx = minmax(s.value)
        det_metrics[outname] = {"min": mn, "max": mx, "mean": mean(s.value), "final": s.value[-1] if s.value else float("nan"), "auc": trapz(s.time, s.value), "series": s}

    with (outdir / "sensitivity_vs_deterministic.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["folder", "order", "tag", "parameter", "side", "output", "delta_min", "rel_delta_min", "delta_max", "rel_delta_max", "delta_mean", "rel_delta_mean", "delta_final", "rel_delta_final", "delta_auc", "rel_delta_auc", "rmse_vs_det", "mae_vs_det", "max_abs_diff_vs_det", "fit_MSE", "fit_R2", "fit_NSE"])
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
                w.writerow([run.folder, run.order, run.tag, run.param, run.side, outname, mn - d["min"], rel_change(mn, d["min"]), mx - d["max"], rel_change(mx, d["max"]), mu - d["mean"], rel_change(mu, d["mean"]), fin - d["final"], rel_change(fin, d["final"]), area - d["auc"], rel_change(area, d["auc"]), rmse, mae, max_abs, fit.get("MSE", ""), fit.get("R2", ""), fit.get("NSE", "")])


def write_calculated_fit_tables(runs: List[RunData], outputs: List[str], outdir: Path, deterministic: RunData) -> None:
    metrics_by_run: Dict[Tuple[str, str], Dict[str, float]] = {}
    with (outdir / "calculated_metrics_vs_deterministic.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["folder", "order", "tag", "parameter", "side", "output", "n_pairs", "calc_MSE", "calc_RMSE", "calc_MAE", "calc_R2", "calc_NSE", "calc_max_abs_diff"])
        for run in runs:
            for outname in outputs:
                ref = deterministic.series.get(outname)
                s = run.series.get(outname)
                if ref is None or s is None:
                    continue
                m = regression_metrics_against_reference(ref, s)
                metrics_by_run[(run.folder, outname)] = m
                w.writerow([run.folder, run.order, run.tag, run.param, run.side, outname, m["n_pairs"], m["MSE"], m["RMSE"], m["MAE"], m["R2"], m["NSE"], m["max_abs_diff"]])
    with (outdir / "calculated_metrics_selected.csv").open("w", newline="", encoding="utf-8") as f:
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
    print("[INFO] Wrote calculated deterministic-reference metrics.")


def write_combined_timeseries(runs: List[RunData], outputs: List[str], outdir: Path, deterministic: RunData) -> Dict[str, Path]:
    paths = {}
    for outname in outputs:
        ref = deterministic.series.get(outname)
        if ref is None:
            continue
        output_file = outdir / f"combined_timeseries_{clean_name_for_file(outname)}.csv"
        valid_runs = [r for r in runs if outname in r.series]
        with output_file.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["time"] + [r.folder for r in valid_runs])
            for t in ref.time:
                w.writerow([t] + [interp_linear(r.series[outname].time, r.series[outname].value, t) for r in valid_runs])
        paths[outname] = output_file
    return paths


def write_rank_table(sensitivity_csv: Path, outdir: Path) -> None:
    rows = []
    with sensitivity_csv.open("r", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                score = abs(float(r.get("rel_delta_min", "nan"))) + abs(float(r.get("rel_delta_max", "nan"))) + abs(float(r.get("rel_delta_auc", "nan"))) + abs(float(r.get("rel_delta_mean", "nan")))
            except Exception:
                score = float("nan")
            if math.isfinite(score):
                r["combined_abs_rel_score"] = score
                rows.append(r)
    rows.sort(key=lambda r: float(r["combined_abs_rel_score"]), reverse=True)
    with (outdir / "rank_sensitivity.csv").open("w", newline="", encoding="utf-8") as f:
        fieldnames = ["rank", "folder", "parameter", "side", "output", "combined_abs_rel_score", "rel_delta_min", "rel_delta_max", "rel_delta_mean", "rel_delta_auc", "rmse_vs_det", "mae_vs_det", "fit_MSE", "fit_R2", "fit_NSE"]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for i, r in enumerate(rows, start=1):
            w.writerow({k: (i if k == "rank" else r.get(k, "")) for k in fieldnames})


def run_gnuplot(gp_path: Path, png_path: Path, label: str, make_png: bool) -> None:
    gnuplot = shutil.which("gnuplot")
    if not make_png or not gnuplot:
        if make_png and not gnuplot:
            print(f"[WARN] gnuplot not found. {label} .gp/.dat written, PNG skipped.")
        return
    if png_path.exists():
        try:
            png_path.unlink()
        except OSError:
            pass
    try:
        subprocess.run([gnuplot, gp_path.name], cwd=gp_path.parent, check=True)
        if png_path.exists() and png_path.stat().st_size > 0:
            print(f"[INFO] Wrote {label} PNG: {png_path.name} ({png_path.stat().st_size} bytes)")
        else:
            print(f"[WARN] {label} PNG is empty or missing after gnuplot: {png_path.name}")
    except subprocess.CalledProcessError as exc:
        print(f"[WARN] gnuplot failed for {label} {gp_path}: {exc}")


def write_gnuplot_scripts(runs: List[RunData], outputs: List[str], combined_paths: Dict[str, Path], outdir: Path, make_png: bool = True) -> None:
    for outname in outputs:
        data_path = combined_paths.get(outname)
        if data_path is None:
            continue
        slug = clean_name_for_file(outname)
        gp_path = outdir / f"plot_{slug}.gp"
        png_path = outdir / f"{slug}.png"
        valid_runs = [r for r in runs if outname in r.series]
        lines = []
        for i, r in enumerate(valid_runs, start=2):
            lw = 3 if "deterministic" in r.folder.lower() else 1.5
            dt = 1 if "deterministic" in r.folder.lower() else (2 + (i % 6))
            lines.append(f'    "{data_path.name}" using 1:{i} with lines lw {lw} dashtype {dt} title "{r.folder.replace("_", " ")}"')
        gp_path.write_text('set datafile separator comma\nset terminal pngcairo size 1400,850 enhanced font "Arial,18"\n' + f'set output "{png_path.name}"\nset grid\nset key outside right top\nset xlabel "Time"\nset ylabel "{outname}"\nset title "{outname} sensitivity comparison"\nplot \\\n' + ', \\\n'.join(lines) + '\n', encoding="utf-8")
        run_gnuplot(gp_path, png_path, "time-series", make_png)


def make_tornado_plot(items: List[dict], dat_path: Path, gp_path: Path, png_path: Path, xlabel: str, ylabel: str, make_png: bool) -> None:
    items.sort(key=lambda x: x["sort_swing"], reverse=True)
    with dat_path.open("w", encoding="utf-8") as f:
        f.write("# y label low high sort_swing low_abs high_abs low_center low_half high_center high_half\n")
        n = len(items)
        for i, item in enumerate(items):
            # Largest item gets the largest y value, so it appears at the TOP.
            y = n - i
            low_abs = abs(item.get("low", 0.0)) if math.isfinite(item.get("low", float("nan"))) else 0.0
            high_abs = abs(item.get("high", 0.0)) if math.isfinite(item.get("high", float("nan"))) else 0.0
            low_half = max(low_abs * 0.5, 1e-14)
            high_half = max(high_abs * 0.5, 1e-14)
            low_center = -low_half
            high_center = high_half
            label = item["label"].replace('"', "'")
            f.write(f'{y} "{label}" {item.get("low", float("nan")):.12g} {item.get("high", float("nan")):.12g} {item["sort_swing"]:.12g} {low_abs:.12g} {high_abs:.12g} {low_center:.12g} {low_half:.12g} {high_center:.12g} {high_half:.12g}\n')

    max_abs = max([abs(item.get("low", 0.0)) for item in items if math.isfinite(item.get("low", float("nan")))] + [abs(item.get("high", 0.0)) for item in items if math.isfinite(item.get("high", float("nan")))] + [1e-12])
    xmax = max_abs * 1.12
    xmin = -xmax
    gp = (
        'set terminal pngcairo size 1500,900 enhanced font "Arial,18"\n'
        f'set output "{png_path.name}"\n'
        'set datafile separator whitespace\n'
        'set style fill solid 0.82 border rgb "black"\n'
        'set key bottom right samplen 1 spacing 1.0\n'
        'unset title\n'
        'set grid x\n'
        'set style line 1 lc rgb "#BDBDBD"\n'
        'set style line 2 lc rgb "#F28E2B"\n'
        f'set xlabel "{xlabel}"\n'
        f'set ylabel "{ylabel}"\n'
        f'set xrange [{xmin:.12g}:{xmax:.12g}]\n'
        f'set yrange [0:{len(items)+1}]\n'
        'set xzeroaxis lw 2 lc rgb "black"\n'
        'set tics out nomirror\n'
        'set border lw 1.2\n'
        'set format x "%g"\n'
        f'plot "{dat_path.name}" using 8:1:9:(0.33):ytic(2) with boxxyerrorbars ls 1 title "L", \\\n'
        f'     "{dat_path.name}" using 10:1:11:(0.33) with boxxyerrorbars ls 2 title "U"\n'
    )
    gp_path.write_text(gp, encoding="utf-8")
    run_gnuplot(gp_path, png_path, "tornado", make_png)


def write_generic_tornado_from_csv(csv_path: Path, outputs: List[str], outdir: Path, value_col: str, table_name: str, file_prefix: str, xlabel: str, make_png: bool = True) -> None:
    if not csv_path.exists():
        print(f"[WARN] {file_prefix} skipped: {csv_path.name} not found.")
        return
    rows = []
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            side = r.get("side", "").upper()
            if side not in {"L", "LOW", "U", "UP", "HIGH", "H"} or r.get("output") not in outputs:
                continue
            try:
                val = float(r.get(value_col, "nan"))
            except Exception:
                val = float("nan")
            if not math.isfinite(val):
                continue
            r["_value"] = val
            rows.append(r)
    if not rows:
        print(f"[WARN] {file_prefix} produced no rows with L/U side labels.")
        return

    grouped: Dict[Tuple[str, str], Dict[str, dict]] = {}
    for r in rows:
        grouped.setdefault((r["output"], r["parameter"]), {})[side_label(r["side"])] = r

    with (outdir / table_name).open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["output", "parameter", "low_folder", "low_value", "high_folder", "high_value", "signed_span", "visual_swing", "dominant_direction"])
        for (outname, param), d in sorted(grouped.items()):
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_value"]) if low else float("nan")
            high_val = float(high["_value"]) if high else float("nan")
            low_abs = abs(low_val) if math.isfinite(low_val) else 0.0
            high_abs = abs(high_val) if math.isfinite(high_val) else 0.0
            if not (math.isfinite(low_val) or math.isfinite(high_val)):
                continue
            signed_span = (high_val if math.isfinite(high_val) else 0.0) - (low_val if math.isfinite(low_val) else 0.0)
            visual_swing = low_abs + high_abs
            if math.isfinite(low_val) and math.isfinite(high_val):
                dom = "High larger" if high_abs > low_abs else "Low larger"
            elif math.isfinite(low_val):
                dom = "Low only"
            else:
                dom = "High only"
            w.writerow([outname, param, low.get("folder", "") if low else "", low_val if math.isfinite(low_val) else "", high.get("folder", "") if high else "", high_val if math.isfinite(high_val) else "", signed_span, visual_swing, dom])
    print(f"[INFO] Wrote tornado table: {table_name}")

    for outname in outputs:
        items = []
        for (o, param), d in grouped.items():
            if o != outname:
                continue
            low = d.get("Low")
            high = d.get("High")
            low_val = float(low["_value"]) if low else float("nan")
            high_val = float(high["_value"]) if high else float("nan")
            low_abs = abs(low_val) if math.isfinite(low_val) else 0.0
            high_abs = abs(high_val) if math.isfinite(high_val) else 0.0
            if low_abs + high_abs <= 0:
                continue
            items.append({"label": pretty_parameter_label(param), "low": low_val, "high": high_val, "sort_swing": low_abs + high_abs})
        if not items:
            print(f"[WARN] No tornado items for output: {outname}")
            continue
        slug = clean_name_for_file(outname)
        make_tornado_plot(items, outdir / f"{file_prefix}_{slug}.dat", outdir / f"plot_{file_prefix}_{slug}.gp", outdir / f"{file_prefix}_{slug}.png", xlabel, "Parameters", make_png)
        print(f"[INFO] Wrote tornado data/plot: {file_prefix}_{slug}")


def write_tornado_tables_and_plots(runs: List[RunData], outputs: List[str], outdir: Path, metric: str = "rel_delta_max", make_png: bool = True) -> None:
    labels = {
        "rel_delta_min": "Minimum response change",
        "rel_delta_max": "Peak response change",
        "rel_delta_mean": "Mean response change",
        "rel_delta_auc": "Integrated response change",
        "rmse_vs_det": "RMSE vs deterministic",
        "max_abs_diff_vs_det": "Maximum absolute difference vs deterministic",
    }
    write_generic_tornado_from_csv(outdir / "sensitivity_vs_deterministic.csv", outputs, outdir, metric, f"tornado_{metric}.csv", f"tornado_{metric}", labels.get(metric, metric), make_png)


def write_calculated_mse_tornado_plots(outputs: List[str], outdir: Path, make_png: bool = True) -> None:
    write_generic_tornado_from_csv(outdir / "calculated_metrics_vs_deterministic.csv", outputs, outdir, "calc_MSE", "tornado_calc_mse.csv", "tornado_calc_mse", "Calculated MSE vs deterministic time series", make_png)


def parse_key_value_fields(line: str) -> Dict[str, str]:
    parts = [x.strip() for x in line.strip().split(';', 1)[-1].split(',')]
    out = {}
    for part in parts:
        if '=' in part:
            k, v = part.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def read_ohq_parameter_values(path: Path) -> Dict[str, float]:
    vals: Dict[str, float] = {}
    if not path.exists():
        return vals
    for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
        line = raw.strip()
        if not line.lower().startswith('create parameter'):
            continue
        fields = parse_key_value_fields(line)
        name = fields.get('name', '')
        v = safe_float(fields.get('value', ''))
        if name and math.isfinite(v):
            vals[normalize_param_name(name)] = v
    return vals


def find_ohq_file(run_dir: Path, preferred_name: str = "") -> Path | None:
    if preferred_name:
        p = run_dir / preferred_name
        if p.exists():
            return p
    files = sorted(run_dir.glob('*.ohq')) or sorted(run_dir.glob('*.OHQ'))
    return files[0] if files else None


def write_mse_log_sensitivity_plots(root: Path, runs: List[RunData], outputs: List[str], outdir: Path, deterministic: RunData, ohq_name: str = "", make_png: bool = True) -> None:
    det_ohq = find_ohq_file(root / deterministic.folder, ohq_name)
    if det_ohq is None:
        print(f"[WARN] MSE log-sensitivity skipped: no .ohq file found in {root / deterministic.folder}")
        return
    det_params = read_ohq_parameter_values(det_ohq)
    if not det_params:
        print(f"[WARN] MSE log-sensitivity skipped: no parameter values found in {det_ohq}")
        return

    run_param_values = {}
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
                print(f"[WARN] Cannot compute ln(MSE): {run.folder}, {outname}, MSE0={mse0}, MSEi={msei}")
                continue
            sens = (math.log(msei) - math.log(mse0)) / (math.log(pi) - math.log(p0))
            if math.isfinite(sens):
                rows.append({"folder": run.folder, "parameter": run.param, "side": side, "output": outname, "p0": p0, "pi": pi, "MSE0": mse0, "MSEi": msei, "dlnMSE_dlnP": sens})

    csv_path = outdir / "tornado_mse_logsens.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        fieldnames = ["folder", "parameter", "side", "output", "p0", "pi", "MSE0", "MSEi", "dlnMSE_dlnP"]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)
    print(f"[INFO] Wrote MSE log-sensitivity table: {csv_path.name}")
    if not rows:
        print("[WARN] MSE log-sensitivity produced no rows. ln(MSE) requires MSE > 0 in fit_measures.txt.")
        return
    write_generic_tornado_from_csv(csv_path, outputs, outdir, "dlnMSE_dlnP", "tornado_mse_logsens_summary.csv", "tornado_mse_logsens", "d(ln(MSE)) / d(ln(p))", make_png)



def write_noisy_mse_log_sensitivity_plots(root: Path, runs: List[RunData], outputs: List[str], outdir: Path, deterministic: RunData, ohq_name: str = "", make_png: bool = True) -> None:
    """
    Same formula as write_mse_log_sensitivity_plots, but writes a separate
    output family for the case where fit_measures.txt was computed against
    the SAME noisy deterministic observation set:

        MSE0 = MSE(deterministic simulation, noisy deterministic observations)
        MSEi = MSE(perturbed simulation,     noisy deterministic observations)

    Files are intentionally named differently so the legacy fit-file
    log-sensitivity outputs are kept unchanged.
    """
    det_ohq = find_ohq_file(root / deterministic.folder, ohq_name)
    if det_ohq is None:
        print(f"[WARN] Noisy MSE log-sensitivity skipped: no .ohq file found in {root / deterministic.folder}")
        return
    det_params = read_ohq_parameter_values(det_ohq)
    if not det_params:
        print(f"[WARN] Noisy MSE log-sensitivity skipped: no parameter values found in {det_ohq}")
        return

    run_param_values = {}
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
            print(f"[WARN] Noisy MSE log-sensitivity: missing parameter value for {run.folder} / {run.param}")
            continue
        if p0 <= 0 or pi <= 0 or abs(math.log(pi) - math.log(p0)) < 1e-30:
            print(f"[WARN] Noisy MSE log-sensitivity: invalid/unchanged p for {run.folder} / {run.param}: p0={p0}, pi={pi}")
            continue
        for outname in outputs:
            mse0 = deterministic.fit.get(outname, {}).get("MSE", float("nan"))
            msei = run.fit.get(outname, {}).get("MSE", float("nan"))
            if not (math.isfinite(mse0) and math.isfinite(msei)):
                continue
            if mse0 <= 0 or msei <= 0:
                print(f"[WARN] Cannot compute noisy ln(MSE): {run.folder}, {outname}, MSE0={mse0}, MSEi={msei}")
                continue
            sens = (math.log(msei) - math.log(mse0)) / (math.log(pi) - math.log(p0))
            if math.isfinite(sens):
                rows.append({
                    "folder": run.folder,
                    "parameter": run.param,
                    "side": side,
                    "output": outname,
                    "p0": p0,
                    "pi": pi,
                    "MSE0_noisy_obs": mse0,
                    "MSEi_noisy_obs": msei,
                    "noisy_dlnMSE_dlnP": sens,
                })

    csv_path = outdir / "tornado_noisy_mse_logsens.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        fieldnames = ["folder", "parameter", "side", "output", "p0", "pi", "MSE0_noisy_obs", "MSEi_noisy_obs", "noisy_dlnMSE_dlnP"]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)
    print(f"[INFO] Wrote noisy MSE log-sensitivity table: {csv_path.name}")
    if not rows:
        print("[WARN] Noisy MSE log-sensitivity produced no rows. ln(MSE) requires MSE > 0 in fit_measures.txt.")
        return
    write_generic_tornado_from_csv(
        csv_path,
        outputs,
        outdir,
        "noisy_dlnMSE_dlnP",
        "tornado_noisy_mse_logsens_summary.csv",
        "tornado_noisy_mse_logsens",
        "Noisy-observation d(ln(MSE)) / d(ln(p))",
        make_png,
    )

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="Root folder containing 1_Deterministic, 2_..., etc.")
    ap.add_argument("--observed-name", default="observedoutput.txt")
    ap.add_argument("--fit-name", default="fit_measures.txt")
    ap.add_argument("--deterministic", default="1_Deterministic")
    ap.add_argument("--outdir", default="SA_Results")
    ap.add_argument("--outputs", nargs="+", default=DEFAULT_OUTPUTS)
    ap.add_argument("--no-gnuplot", action="store_true")
    ap.add_argument("--no-mse-log-sensitivity", action="store_true")
    ap.add_argument("--no-noisy-mse-log-sensitivity", action="store_true", help="Skip separate noisy-observation fit-file log-sensitivity outputs.")
    ap.add_argument("--ohq-name", default="")
    choices = ["rel_delta_min", "rel_delta_max", "rel_delta_mean", "rel_delta_auc", "rmse_vs_det", "max_abs_diff_vs_det"]
    ap.add_argument("--tornado-metric", default=None, choices=choices)
    ap.add_argument("--tornado-metrics", nargs="+", default=None, choices=choices + ["all"])
    args = ap.parse_args()

    root = Path(args.root).resolve()
    outdir = root / args.outdir
    outdir.mkdir(parents=True, exist_ok=True)

    runs = load_runs(root, args.observed_name, args.fit_name)
    if not runs:
        raise SystemExit(f"No folders containing {args.observed_name} found under {root}")
    deterministic = next((r for r in runs if r.folder == args.deterministic), runs[0])
    if deterministic.folder != args.deterministic:
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

    make_png = not args.no_gnuplot
    write_fit_tables(runs, outputs, outdir)
    write_summary(runs, outputs, outdir, deterministic)
    write_calculated_fit_tables(runs, outputs, outdir, deterministic)
    write_calculated_mse_tornado_plots(outputs, outdir, make_png=make_png)
    combined = write_combined_timeseries(runs, outputs, outdir, deterministic)
    write_rank_table(outdir / "sensitivity_vs_deterministic.csv", outdir)
    write_gnuplot_scripts(runs, outputs, combined, outdir, make_png=make_png)

    if args.tornado_metrics:
        tornado_metrics = args.tornado_metrics
    elif args.tornado_metric:
        tornado_metrics = [args.tornado_metric]
    else:
        tornado_metrics = ["max_abs_diff_vs_det", "rel_delta_max", "rel_delta_min"]
    if "all" in tornado_metrics:
        tornado_metrics = ["max_abs_diff_vs_det", "rel_delta_max", "rel_delta_min"]
    tornado_metrics = list(dict.fromkeys(tornado_metrics))
    for metric in tornado_metrics:
        write_tornado_tables_and_plots(runs, outputs, outdir, metric=metric, make_png=make_png)

    if not args.no_mse_log_sensitivity:
        write_mse_log_sensitivity_plots(root, runs, outputs, outdir, deterministic, ohq_name=args.ohq_name, make_png=make_png)

    if not args.no_noisy_mse_log_sensitivity:
        write_noisy_mse_log_sensitivity_plots(root, runs, outputs, outdir, deterministic, ohq_name=args.ohq_name, make_png=make_png)

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
    for metric in tornado_metrics:
        print(f"     tornado_{metric}.csv")
    if not args.no_mse_log_sensitivity:
        print("     tornado_mse_logsens.csv")
        for o in outputs:
            print(f"     tornado_mse_logsens_{clean_name_for_file(o)}.png")
    if not args.no_noisy_mse_log_sensitivity:
        print("     tornado_noisy_mse_logsens.csv")
        print("     tornado_noisy_mse_logsens_summary.csv")
        for o in outputs:
            print(f"     tornado_noisy_mse_logsens_{clean_name_for_file(o)}.png")
    for o in outputs:
        print(f"     combined_timeseries_{clean_name_for_file(o)}.csv")
        print(f"     plot_{clean_name_for_file(o)}.gp")


if __name__ == "__main__":
    main()
