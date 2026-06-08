#!/usr/bin/env python3
"""
build_fitness_history_wetland.py

Wetland version of build_fitness_history.py.
Reads Wetland_assimilation/outputs/calibration/ga_output_merged.txt and writes
Wetland_assimilation/outputs/calibration/fitness_history_wetland.csv.

Active weighted calibration channels are expected to match the Wetland.ohq
observation names exactly:
  - Wetland inlet stage (m)
  - Wetland mid-stage (m)
  - Wetland outlet stage (m)
  - Wetland outflow (m3/day)
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

DEFAULT_CALIB_DIR = Path("Wetland_assimilation/outputs/calibration")
DEFAULT_GA_PATH   = DEFAULT_CALIB_DIR / "ga_output_merged.txt"
DEFAULT_OUT_PATH  = DEFAULT_CALIB_DIR / "fitness_history_wetland.csv"

DEFAULT_KERNEL_DELTA0 = 30.0
DEFAULT_KERNEL_TAU    = 1.0
DEFAULT_KERNEL_ALPHA  = 1.0
DEFAULT_OBS_INTERVAL_HOURS = 1.0

ACTIVE_CHANNELS = [
    ("Wetland inlet stage (m)", "WetlandInletStage"),
    ("Wetland mid-stage (m)", "WetlandMidStage"),
    ("Wetland outlet stage (m)", "WetlandOutletStage"),
    ("Wetland outflow (m3/day)", "WetlandOutflow"),
]

CYCLE_RE = re.compile(
    r"^===\s*Cycle\s+(\d+)\s*\|\s*timestamp\s+(\S+\s+\S+)\s*\|\s*t_now=(\S+)\s*===\s*$"
)
GENERATION_RE = re.compile(r"^Generation:\s*(\d+)\s*$")
HEADER_PREFIX = "ID,"


@dataclass
class Generation:
    index: int
    header: list[str] = field(default_factory=list)
    rows: list[list[str]] = field(default_factory=list)


@dataclass
class Cycle:
    index: int
    timestamp: str
    t_now: float
    generations: list[Generation] = field(default_factory=list)


def parse_cycles(path: Path) -> Iterator[Cycle]:
    current_cycle: Cycle | None = None
    current_gen: Generation | None = None
    expect_header_next = False

    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw_line in fh:
            line = raw_line.rstrip("\n")
            if not line.strip():
                continue

            m = CYCLE_RE.match(line)
            if m:
                if current_cycle is not None:
                    yield current_cycle
                current_cycle = Cycle(
                    index=int(m.group(1)),
                    timestamp=m.group(2),
                    t_now=float(m.group(3)),
                )
                current_gen = None
                expect_header_next = False
                continue

            m = GENERATION_RE.match(line)
            if m:
                if current_cycle is None:
                    continue
                current_gen = Generation(index=int(m.group(1)))
                current_cycle.generations.append(current_gen)
                expect_header_next = True
                continue

            if line.startswith("Final Enhancements"):
                expect_header_next = False
                continue

            if current_gen is None:
                continue

            if expect_header_next and line.startswith(HEADER_PREFIX):
                fields = [f.strip() for f in line.split(",")]
                if fields and fields[-1] == "":
                    fields = fields[:-1]
                current_gen.header = fields
                expect_header_next = False
                continue

            fields = [f.strip() for f in line.split(",")]
            current_gen.rows.append(fields)

    if current_cycle is not None:
        yield current_cycle


def column_index(header: list[str], name: str) -> int:
    try:
        return header.index(name)
    except ValueError as exc:
        raise KeyError(f"Column {name!r} not found. Available columns: {header}") from exc


def row_index_for(header_idx: int, rank_boundary: int) -> int:
    # OHQ GA rows include one extra empty field after Rank that is absent in header.
    return header_idx if header_idx <= rank_boundary else header_idx + 1


def best_individual(gen: Generation) -> tuple[list[str], float]:
    if not gen.rows:
        raise ValueError(f"Generation {gen.index} has no rows")
    if not gen.header:
        raise ValueError(f"Generation {gen.index} has no header")
    lik_idx = column_index(gen.header, "likelihood")
    best_row: list[str] | None = None
    best_lik = float("inf")
    for row in gen.rows:
        try:
            lik = float(row[lik_idx])
        except (ValueError, IndexError):
            continue
        if lik < best_lik:
            best_lik = lik
            best_row = row
    if best_row is None:
        raise ValueError(f"Generation {gen.index}: no parseable likelihood values")
    return best_row, best_lik


def extract_channel_metrics(header: list[str], row: list[str], channel_label: str) -> tuple[float, float, float]:
    rank_boundary = column_index(header, "Rank")

    def fetch(name: str) -> float:
        h_idx = column_index(header, name)
        r_idx = row_index_for(h_idx, rank_boundary)
        return float(row[r_idx])

    return (
        fetch(f"{channel_label}_MSE"),
        fetch(f"{channel_label}_R2"),
        fetch(f"{channel_label}_NSE"),
    )


def buffer_sum_w_per_channel(
    t_now: float,
    t_buffer_start: float,
    obs_interval_hours: float,
    Delta0: float,
    tau: float,
    alpha: float,
) -> tuple[float, int]:
    if t_now < t_buffer_start:
        return 0.0, 0
    dt_days = obs_interval_hours / 24.0
    if dt_days <= 0.0:
        raise ValueError("obs_interval_hours must be positive")

    span_days = t_now - t_buffer_start
    n_obs = int(math.floor(span_days / dt_days)) + 1
    kernel_active = (Delta0 > 0.0) and (tau > 0.0)

    sum_w = 0.0
    for i in range(n_obs):
        delta = span_days - i * dt_days
        if delta < 0.0:
            delta = 0.0
        if not kernel_active or delta < Delta0:
            w = 1.0
        else:
            w = (1.0 + math.log(delta / Delta0) / tau) ** (-alpha)
        sum_w += w
    return sum_w, n_obs


def main() -> int:
    ap = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ga", type=Path, default=DEFAULT_GA_PATH)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT_PATH)
    ap.add_argument("--kernel-delta0", type=float, default=DEFAULT_KERNEL_DELTA0)
    ap.add_argument("--kernel-tau", type=float, default=DEFAULT_KERNEL_TAU)
    ap.add_argument("--kernel-alpha", type=float, default=DEFAULT_KERNEL_ALPHA)
    ap.add_argument("--obs-interval-hours", type=float, default=DEFAULT_OBS_INTERVAL_HOURS)
    ap.add_argument("--buffer-start", type=float, default=None)
    args = ap.parse_args()

    if not args.ga.exists():
        print(f"[error] GA archive not found: {args.ga}", file=sys.stderr)
        print(f"        cwd is: {Path.cwd()}", file=sys.stderr)
        return 2

    args.out.parent.mkdir(parents=True, exist_ok=True)
    n_channels = len(ACTIVE_CHANNELS)

    out_header = ["cycle", "timestamp", "t_now", "likelihood"]
    for _, short in ACTIVE_CHANNELS:
        out_header += [f"{short}_MSE", f"{short}_R2", f"{short}_NSE"]
    out_header += [
        "buffer_obs_per_channel",
        "sum_w_per_channel",
        "sum_w_total",
        "likelihood_per_w",
    ]

    n_cycles = 0
    n_skipped = 0
    buffer_start: float | None = args.buffer_start

    with args.out.open("w", newline="", encoding="utf-8") as out_fh:
        writer = csv.writer(out_fh)
        writer.writerow(out_header)

        for cycle in parse_cycles(args.ga):
            if not cycle.generations:
                print(f"[warn] Cycle {cycle.index}: no generations, skipping", file=sys.stderr)
                n_skipped += 1
                continue

            final_gen = max(cycle.generations, key=lambda g: g.index)
            try:
                row, lik = best_individual(final_gen)
            except (KeyError, ValueError) as exc:
                print(f"[warn] Cycle {cycle.index}: {exc}", file=sys.stderr)
                n_skipped += 1
                continue

            if buffer_start is None:
                buffer_start = cycle.t_now

            out_row: list[object] = [cycle.index, cycle.timestamp, f"{cycle.t_now:.6f}", f"{lik:.6e}"]
            for label, _ in ACTIVE_CHANNELS:
                try:
                    mse, r2, nse = extract_channel_metrics(final_gen.header, row, label)
                except (KeyError, IndexError, ValueError) as exc:
                    print(f"[warn] Cycle {cycle.index} channel {label!r}: {exc}", file=sys.stderr)
                    mse = r2 = nse = float("nan")
                out_row += [f"{mse:.6e}", f"{r2:.6e}", f"{nse:.6e}"]

            sum_w_pc, n_obs_pc = buffer_sum_w_per_channel(
                t_now=cycle.t_now,
                t_buffer_start=buffer_start,
                obs_interval_hours=args.obs_interval_hours,
                Delta0=args.kernel_delta0,
                tau=args.kernel_tau,
                alpha=args.kernel_alpha,
            )
            sum_w_total = sum_w_pc * n_channels
            lik_per_w = lik / sum_w_total if sum_w_total > 0 else float("nan")
            out_row += [n_obs_pc, f"{sum_w_pc:.6e}", f"{sum_w_total:.6e}", f"{lik_per_w:.6e}"]
            writer.writerow(out_row)
            n_cycles += 1

    print(f"Wrote {n_cycles} cycle rows to {args.out} ({n_skipped} skipped)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
