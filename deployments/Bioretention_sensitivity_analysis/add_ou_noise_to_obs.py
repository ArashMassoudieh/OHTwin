#!/usr/bin/env python3
"""
Add relative Ornstein-Uhlenbeck (OU) noise to OHTwin/OpenHydroQual observation CSVs.

Default behavior matches the uploaded Bioretention truth config:
  - Pond water depth: 10%
  - Underdrain flow: 10%
  - Soil Moisture: 1%
  - OU autocorrelation time: 6 hr

Input CSV format expected:
  t, Obs_<output name>_expression
  ...

The time column is assumed to be in days, which matches OHQ-style serial day output.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Dict, Tuple

import numpy as np
import pandas as pd


DEFAULT_NOISE_SIGMA = {
    "Pond water depth": 0.10,
    "Pond water depth (m)": 0.10,
    "Underdrain flow": 0.10,
    "Underdrain flow (m3/day)": 0.10,
    "Soil Moisture": 0.01,
}


def parse_duration_to_days(text: str | float | int | None, default_days: float = 6.0 / 24.0) -> float:
    """Parse strings like '6hr', '1day', '30min', '3600s' into days."""
    if text is None:
        return default_days
    if isinstance(text, (int, float)):
        return float(text)

    s = str(text).strip().lower()
    m = re.match(r"^\s*([0-9]*\.?[0-9]+)\s*([a-z]+)?\s*$", s)
    if not m:
        return default_days

    value = float(m.group(1))
    unit = m.group(2) or "day"

    if unit in {"d", "day", "days"}:
        return value
    if unit in {"h", "hr", "hrs", "hour", "hours"}:
        return value / 24.0
    if unit in {"m", "min", "mins", "minute", "minutes"}:
        return value / (24.0 * 60.0)
    if unit in {"s", "sec", "secs", "second", "seconds"}:
        return value / (24.0 * 3600.0)

    return default_days


def canonical_name(name: str) -> str:
    """Normalize output names so config keys with/without units can match."""
    s = name.strip()
    s = re.sub(r"^Obs_", "", s)
    s = re.sub(r"_expression$", "", s)
    s = re.sub(r"\([^)]*\)", "", s)  # remove units
    s = re.sub(r"[_\s]+", " ", s)
    return s.strip().lower()


def extract_output_name(value_col: str) -> str:
    s = value_col.strip()
    s = re.sub(r"^Obs_", "", s)
    s = re.sub(r"_expression$", "", s)
    return s.strip()


def load_noise_settings(config_path: Path | None) -> Tuple[Dict[str, float], float]:
    """Return noise fractions by output name and OU correlation time in days."""
    noise = dict(DEFAULT_NOISE_SIGMA)
    tau_days = 6.0 / 24.0

    if config_path is None:
        return noise, tau_days

    cfg = json.loads(config_path.read_text(encoding="utf-8"))
    obs = cfg.get("observations", {})

    cfg_noise = obs.get("noise_sigma", {})
    if isinstance(cfg_noise, dict):
        for k, v in cfg_noise.items():
            try:
                noise[str(k)] = float(v)
            except Exception:
                pass
    elif isinstance(cfg_noise, (int, float)):
        noise["default"] = float(cfg_noise)

    tau_days = parse_duration_to_days(obs.get("noise_correlation_time", None), tau_days)
    return noise, tau_days


def get_noise_fraction(output_name: str, noise: Dict[str, float]) -> float:
    """Find output-specific noise fraction from config dictionary."""
    key = canonical_name(output_name)

    # Exact/canonical match first.
    for k, v in noise.items():
        if canonical_name(k) == key:
            return float(v)

    # Partial match fallback, e.g. "Pond water depth (m)" vs "Pond water depth".
    for k, v in noise.items():
        ck = canonical_name(k)
        if ck and (ck in key or key in ck):
            return float(v)

    return float(noise.get("default", 0.0))


def robust_output_scale(y: np.ndarray) -> float:
    """Representative scale used only if --min-scale-frac > 0."""
    finite = np.asarray(y[np.isfinite(y)], dtype=float)
    if finite.size == 0:
        return 0.0
    q05, q95 = np.nanpercentile(finite, [5, 95])
    scale = q95 - q05
    if not np.isfinite(scale) or scale <= 0:
        scale = np.nanmedian(np.abs(finite))
    if not np.isfinite(scale) or scale <= 0:
        scale = np.nanmax(np.abs(finite))
    return float(scale) if np.isfinite(scale) else 0.0


def add_relative_ou_noise(
    t_days: np.ndarray,
    y: np.ndarray,
    frac: float,
    tau_days: float,
    rng: np.random.Generator,
    min_scale_frac: float = 0.0,
    nonnegative: bool = True,
) -> Tuple[np.ndarray, np.ndarray]:
    """
    Add OU noise with local standard deviation:
        sigma_i = frac * max(abs(y_i), min_scale_frac * robust_scale(y))

    OU update:
        e_i = rho_i e_{i-1} + sqrt(1-rho_i^2) sigma_i Z_i
        rho_i = exp(-dt_i/tau)
    """
    y = np.asarray(y, dtype=float)
    t_days = np.asarray(t_days, dtype=float)

    out = y.copy()
    err = np.zeros_like(y, dtype=float)

    if frac <= 0 or len(y) == 0:
        return out, err

    scale_floor = max(0.0, float(min_scale_frac)) * robust_output_scale(y)

    for i in range(len(y)):
        if not np.isfinite(y[i]):
            out[i] = y[i]
            err[i] = np.nan
            continue

        sigma_i = frac * max(abs(y[i]), scale_floor)

        if i == 0 or not np.isfinite(t_days[i]) or not np.isfinite(t_days[i - 1]) or tau_days <= 0:
            rho = 0.0
        else:
            dt = max(0.0, t_days[i] - t_days[i - 1])
            rho = math.exp(-dt / tau_days)

        innovation_sd = math.sqrt(max(0.0, 1.0 - rho * rho)) * sigma_i
        prev = 0.0 if i == 0 or not np.isfinite(err[i - 1]) else err[i - 1]
        err[i] = rho * prev + innovation_sd * rng.normal()
        out[i] = y[i] + err[i]

        if nonnegative:
            out[i] = max(0.0, out[i])

    return out, err


def process_csv(
    csv_path: Path,
    outdir: Path,
    noise: Dict[str, float],
    tau_days: float,
    seed: int,
    min_scale_frac: float,
    nonnegative: bool,
) -> Path:
    df = pd.read_csv(csv_path)
    df.columns = [c.strip() for c in df.columns]

    if len(df.columns) < 2:
        raise ValueError(f"{csv_path} needs at least two columns: time and value.")

    time_col = df.columns[0]
    value_col = df.columns[1]
    output_name = extract_output_name(value_col)

    frac = get_noise_fraction(output_name, noise)
    rng = np.random.default_rng(seed)

    t = pd.to_numeric(df[time_col], errors="coerce").to_numpy(dtype=float)
    y = pd.to_numeric(df[value_col], errors="coerce").to_numpy(dtype=float)

    noisy, err = add_relative_ou_noise(
        t_days=t,
        y=y,
        frac=frac,
        tau_days=tau_days,
        rng=rng,
        min_scale_frac=min_scale_frac,
        nonnegative=nonnegative,
    )

    out = pd.DataFrame({
        time_col: t,
        value_col: y,
        f"{value_col}_OU_noise": err,
        f"{value_col}_noisy": noisy,
    })

    outdir.mkdir(parents=True, exist_ok=True)
    out_path = outdir / f"{csv_path.stem}_OU_noisy.csv"
    out.to_csv(out_path, index=False)

    print(
        f"[OK] {csv_path.name}: output='{output_name}', "
        f"sigma_fraction={frac:g}, tau_days={tau_days:g}, wrote {out_path}"
    )
    return out_path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", help="Observation CSV files.")
    ap.add_argument("--config", default=None, help="Truth config JSON with observations.noise_sigma.")
    ap.add_argument("--outdir", default="noisy_observations")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument(
        "--min-scale-frac",
        type=float,
        default=0.0,
        help=(
            "Optional lower bound for relative noise scale. "
            "0 means sigma = fraction*abs(output). "
            "Example: 1e-3 prevents exactly zero noise during zero-flow periods."
        ),
    )
    ap.add_argument("--allow-negative", action="store_true", help="Do not clip noisy values at zero.")
    args = ap.parse_args()

    config_path = Path(args.config) if args.config else None
    noise, tau_days = load_noise_settings(config_path)

    outdir = Path(args.outdir)
    for k, csv_name in enumerate(args.csvs):
        process_csv(
            csv_path=Path(csv_name),
            outdir=outdir,
            noise=noise,
            tau_days=tau_days,
            seed=args.seed + k,
            min_scale_frac=args.min_scale_frac,
            nonnegative=not args.allow_negative,
        )


if __name__ == "__main__":
    main()
