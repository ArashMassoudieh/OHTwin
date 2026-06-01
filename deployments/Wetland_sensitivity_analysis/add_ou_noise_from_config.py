#!/usr/bin/env python3
"""
Add MULTIPLICATIVE (log-normal) Ornstein-Uhlenbeck (OU) noise to
OHTwin/OpenHydroQual observation CSVs, taking noise percentages directly
from config.json:

    observations.noise_sigma
    observations.noise_correlation_time

Noise model matches C++ TimeSeries<T>::add_OU_noise:

    y_noisy_i = y_i * exp(sigma_i * eps_i)

where eps is a unit-variance OU process:

    eps_i = rho_i * eps_{i-1} + sqrt(1-rho_i^2) * Z_i
    rho_i = exp(-dt_i/tau)
    eps_0 ~ N(0,1)

Notes:
  - sigma=0.10 means about 10% log-space multiplicative noise.
  - Exact zeros remain zero: 0 * exp(.) = 0.
  - The first CSV column is time in DAYS.
  - The observation name is parsed from headers like:
        Obs_Wetland inflow (m3/day)_expression
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
    s = str(name).strip()
    s = re.sub(r"^Obs_", "", s)
    s = re.sub(r"_expression$", "", s)
    s = re.sub(r"\([^)]*\)", "", s)  # remove units
    s = re.sub(r"[_\s]+", " ", s)
    return s.strip().lower()


def extract_output_name(value_col: str) -> str:
    """Extract observation name from a CSV value-column header."""
    s = str(value_col).strip()
    s = re.sub(r"^Obs_", "", s)
    s = re.sub(r"_expression$", "", s)
    return s.strip()


def load_noise_settings(config_path: Path) -> Tuple[Dict[str, float], float]:
    """Load noise sigma dictionary and OU correlation time from config.json."""
    cfg = json.loads(config_path.read_text(encoding="utf-8"))
    obs = cfg.get("observations", {})

    raw_noise = obs.get("noise_sigma", {})
    noise: Dict[str, float] = {}

    if isinstance(raw_noise, dict):
        for k, v in raw_noise.items():
            try:
                noise[str(k)] = float(v)
            except Exception:
                print(f"[WARN] Skipping non-numeric noise_sigma entry: {k}={v}")
    elif isinstance(raw_noise, (int, float)):
        noise["default"] = float(raw_noise)

    if "default" not in noise:
        noise["default"] = 0.0

    tau_days = parse_duration_to_days(obs.get("noise_correlation_time", None), 6.0 / 24.0)
    return noise, tau_days


def get_noise_fraction(output_name: str, noise: Dict[str, float]) -> Tuple[float, str]:
    """Find output-specific noise fraction from config dictionary."""
    key = canonical_name(output_name)

    # Exact/canonical match first.
    for k, v in noise.items():
        if k == "default":
            continue
        if canonical_name(k) == key:
            return float(v), k

    # Partial match fallback, e.g. with/without units.
    for k, v in noise.items():
        if k == "default":
            continue
        ck = canonical_name(k)
        if ck and (ck in key or key in ck):
            return float(v), k

    return float(noise.get("default", 0.0)), "default"


def add_multiplicative_ou_noise(
    t_days: np.ndarray,
    y: np.ndarray,
    frac: float,
    tau_days: float,
    rng: np.random.Generator,
) -> Tuple[np.ndarray, np.ndarray]:
    """
    Multiplicative log-normal OU noise:

        y_noisy_i = y_i * exp(frac * eps_i)

    Returns:
        noisy_values, realized_perturbation = noisy - clean
    """
    y = np.asarray(y, dtype=float)
    t_days = np.asarray(t_days, dtype=float)

    out = y.copy()
    err = np.zeros_like(y, dtype=float)

    if frac <= 0 or len(y) == 0:
        return out, err

    eps = rng.normal()  # stationary N(0,1) start

    for i in range(len(y)):
        if i > 0:
            dt = t_days[i] - t_days[i - 1]
            if tau_days <= 0 or not np.isfinite(dt) or dt <= 0:
                eps = rng.normal()
            else:
                rho = math.exp(-dt / tau_days)
                eps = rho * eps + math.sqrt(max(0.0, 1.0 - rho * rho)) * rng.normal()

        if not np.isfinite(y[i]):
            out[i] = y[i]
            err[i] = np.nan
            continue

        out[i] = y[i] * math.exp(frac * eps)
        err[i] = out[i] - y[i]

    return out, err


def process_csv(
    csv_path: Path,
    outdir: Path,
    noise: Dict[str, float],
    tau_days: float,
    seed: int,
) -> Path:
    df = pd.read_csv(csv_path)
    df.columns = [str(c).strip() for c in df.columns]

    if len(df.columns) < 2:
        raise ValueError(f"{csv_path} needs at least two columns: time and observation value.")

    time_col = df.columns[0]
    value_col = df.columns[1]
    output_name = extract_output_name(value_col)

    frac, matched_key = get_noise_fraction(output_name, noise)
    rng = np.random.default_rng(seed)

    t = pd.to_numeric(df[time_col], errors="coerce").to_numpy(dtype=float)
    y = pd.to_numeric(df[value_col], errors="coerce").to_numpy(dtype=float)

    noisy, err = add_multiplicative_ou_noise(
        t_days=t,
        y=y,
        frac=frac,
        tau_days=tau_days,
        rng=rng,
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
        f"matched_config='{matched_key}', sigma={frac:g} ({100.0*frac:g}%), "
        f"tau_days={tau_days:g}, wrote {out_path}"
    )
    return out_path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", help="Observation CSV files.")
    ap.add_argument("--config", required=True, help="config.json containing observations.noise_sigma.")
    ap.add_argument("--outdir", default="noisy_observations")
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args()

    config_path = Path(args.config)
    noise, tau_days = load_noise_settings(config_path)

    print("[INFO] Noise percentages loaded from config:")
    for k, v in noise.items():
        print(f"  - {k}: {100.0 * float(v):g}%")
    print(f"[INFO] OU correlation time: {tau_days:g} days")

    outdir = Path(args.outdir)
    for k, csv_name in enumerate(args.csvs):
        process_csv(
            csv_path=Path(csv_name),
            outdir=outdir,
            noise=noise,
            tau_days=tau_days,
            seed=args.seed + k,
        )


if __name__ == "__main__":
    main()
