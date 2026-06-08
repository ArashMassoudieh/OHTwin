#!/usr/bin/env python3
"""
build_fitness_history.py

Reads ga_output_merged.txt (the cumulative GA archive from OHTwin's
DTAssimilation) and produces a per-cycle fitness summary CSV that mirrors
the structure of parameter_history.csv.

For each cycle, the "best individual" is defined as the row in the FINAL
generation (the highest-numbered "Generation:" block before the next
"=== Cycle ..." header) with the lowest `likelihood` value. This matches
OHTwin's CalcMisfit() semantics where lower = better.

Outputs one row per cycle with:
    - cycle, timestamp, t_now           (cycle metadata)
    - likelihood                        (CalcMisfit return value, the
                                         scalar GA fitness)
    - For each of the three calibrated channels (Pond water depth,
      Soil Moisture, Underdrain flow):
        - <channel>_MSE
        - <channel>_R2
        - <channel>_NSE

Diagnostic (inactive) channels are skipped — they hold zeros because
they don't contribute to CalcMisfit().

Default I/O paths assume the script is run from the `deployments/`
directory, with input and output both under
    Bioretention_assimilation/outputs/calibration/
Override with --ga / --out if you have a different layout.

Usage:
    # From deployments/, using defaults:
    python build_fitness_history.py

    # Or override explicitly:
    python build_fitness_history.py \
        --ga path/to/ga_output_merged.txt \
        --out path/to/fitness_history.csv
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

# Default I/O paths, relative to the current working directory. Assumes
# the script is invoked from the `deployments/` directory.
DEFAULT_CALIB_DIR = Path("Bioretention_assimilation/outputs/calibration")
DEFAULT_GA_PATH   = DEFAULT_CALIB_DIR / "ga_output_merged.txt"
DEFAULT_OUT_PATH  = DEFAULT_CALIB_DIR / "fitness_history.csv"

# Kernel parameters (must match those set on each Observation block in the
# OHQ model). The WLS weight kernel is:
#     w(delta) = 1                                   if delta < Delta0
#     w(delta) = (1 + ln(delta/Delta0)/tau)^{-alpha} otherwise
# Time units: days (matches OHQ day-serial).
DEFAULT_KERNEL_DELTA0 = 30.0
DEFAULT_KERNEL_TAU    = 1.0
DEFAULT_KERNEL_ALPHA  = 1.0

# Assimilation buffer policy: observations are accumulated indefinitely
# (no buffer pruning) from the simulated time of cycle 0 up to t_now of
# the current cycle, at a fixed sampling interval. Each active channel
# contributes its own sequence of observations; sum_w is summed within a
# channel, and the same kernel is applied per channel.
DEFAULT_OBS_INTERVAL_HOURS = 1.0

# Channels actually used by CalcMisfit() (i.e. those with assigned sigmas
# in parameter_history.csv: Std_PondWaterDepth, Std_SoilMoisture,
# Std_UnderdrainFlow). The labels here must match the column header text
# in ga_output_merged.txt exactly (whitespace and parentheses included).
ACTIVE_CHANNELS = [
    ("Pond water depth (m)", "PondWaterDepth"),
    ("Soil Moisture",        "SoilMoisture"),
    ("Underdrain flow (m3/day)", "UnderdrainFlow"),
]

# Cycle header: "=== Cycle 0 | timestamp 2026-05-20 16:05:27 | t_now=43840.000000 ==="
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
    """Stream-parse ga_output_merged.txt into Cycle objects."""
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
                    # Stray Generation line before any cycle header — skip.
                    continue
                current_gen = Generation(index=int(m.group(1)))
                current_cycle.generations.append(current_gen)
                expect_header_next = True
                continue

            if line.startswith("Final Enhancements"):
                expect_header_next = False
                continue

            if current_gen is None:
                # Lines outside any generation — ignore.
                continue

            if expect_header_next and line.startswith(HEADER_PREFIX):
                # The header has a trailing comma which yields an empty
                # field at the end; strip it.
                fields = [f.strip() for f in line.split(",")]
                if fields and fields[-1] == "":
                    fields = fields[:-1]
                current_gen.header = fields
                expect_header_next = False
                continue

            # Data row. Note: data rows have one more field than the
            # header because of an extra empty field inserted after Rank;
            # do NOT strip trailing empties here.
            fields = [f.strip() for f in line.split(",")]
            current_gen.rows.append(fields)

    if current_cycle is not None:
        yield current_cycle


def column_index(header: list[str], name: str) -> int:
    try:
        return header.index(name)
    except ValueError:
        raise KeyError(
            f"Column {name!r} not found in header. Available: {header}"
        )


# OHQ's GA writer emits an extra empty field between the `Rank` column and
# the per-channel MSE/R2/NSE block in every data row, but not in the header.
# So for any column at or after RANK_BOUNDARY in the header, we look up the
# corresponding value at index+1 in the data row.
def row_index_for(header_idx: int, rank_boundary: int) -> int:
    return header_idx if header_idx <= rank_boundary else header_idx + 1


def best_individual(gen: Generation) -> tuple[list[str], float]:
    """Return (row, likelihood) for the row with the lowest likelihood."""
    if not gen.rows:
        raise ValueError(f"Generation {gen.index} has no rows")
    if not gen.header:
        raise ValueError(f"Generation {gen.index} has no header")
    lik_idx = column_index(gen.header, "likelihood")
    best_row = None
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


def extract_channel_metrics(
    header: list[str], row: list[str], channel_label: str
) -> tuple[float, float, float]:
    """Return (MSE, R2, NSE) for one channel, accounting for the empty
    field that GA output inserts after the Rank column in data rows."""
    rank_boundary = column_index(header, "Rank")

    def fetch(name: str) -> float:
        h_idx = column_index(header, name)
        r_idx = row_index_for(h_idx, rank_boundary)
        return float(row[r_idx])

    mse = fetch(f"{channel_label}_MSE")
    r2  = fetch(f"{channel_label}_R2")
    nse = fetch(f"{channel_label}_NSE")
    return mse, r2, nse


def buffer_sum_w_per_channel(
    t_now: float,
    t_buffer_start: float,
    obs_interval_hours: float,
    Delta0: float,
    tau: float,
    alpha: float,
) -> tuple[float, int]:
    """
    Compute the per-channel sum of WLS kernel weights and observation
    count for an unbounded buffer that started accumulating at
    `t_buffer_start` (in OHQ day-serial) and has reached `t_now`.

    The kernel matches `weighted_mse` in TimeSeries.hpp:
        w(delta) = 1                                   if delta < Delta0
        w(delta) = (1 + ln(delta/Delta0)/tau)^{-alpha} otherwise
    where delta = t_now - t_obs, expressed in days.

    Observations are placed at t_buffer_start, t_buffer_start + dt,
    ..., up to and including t_now (so the most recent observation has
    delta = 0 and gets weight 1). dt = obs_interval_hours / 24 days.

    Returns (sum_w, n_obs).
    """
    if t_now < t_buffer_start:
        return 0.0, 0

    dt_days = obs_interval_hours / 24.0
    if dt_days <= 0.0:
        raise ValueError("obs_interval_hours must be positive")

    span_days = t_now - t_buffer_start
    n_obs = int(math.floor(span_days / dt_days)) + 1   # inclusive of both ends

    kernel_active = (Delta0 > 0.0) and (tau > 0.0)

    sum_w = 0.0
    for i in range(n_obs):
        # Place obs i at t_buffer_start + i*dt; delta is its age relative
        # to t_now. The newest observation (i = n_obs - 1) sits at or just
        # before t_now and contributes weight 1.
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
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--ga", type=Path, default=DEFAULT_GA_PATH,
                    help=f"Path to ga_output_merged.txt "
                         f"(default: {DEFAULT_GA_PATH})")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT_PATH,
                    help=f"Output CSV path "
                         f"(default: {DEFAULT_OUT_PATH})")
    ap.add_argument("--kernel-delta0", type=float,
                    default=DEFAULT_KERNEL_DELTA0,
                    help=f"WLS kernel Delta0 in days "
                         f"(default: {DEFAULT_KERNEL_DELTA0})")
    ap.add_argument("--kernel-tau", type=float,
                    default=DEFAULT_KERNEL_TAU,
                    help=f"WLS kernel tau (default: {DEFAULT_KERNEL_TAU})")
    ap.add_argument("--kernel-alpha", type=float,
                    default=DEFAULT_KERNEL_ALPHA,
                    help=f"WLS kernel alpha (default: {DEFAULT_KERNEL_ALPHA})")
    ap.add_argument("--obs-interval-hours", type=float,
                    default=DEFAULT_OBS_INTERVAL_HOURS,
                    help=f"Observation sampling interval in hours "
                         f"(default: {DEFAULT_OBS_INTERVAL_HOURS})")
    ap.add_argument("--buffer-start", type=float, default=None,
                    help="OHQ day-serial of the oldest buffered observation. "
                         "If omitted, uses t_now of cycle 0.")
    args = ap.parse_args()

    if not args.ga.exists():
        print(f"[error] GA archive not found: {args.ga}", file=sys.stderr)
        print(f"        cwd is: {Path.cwd()}", file=sys.stderr)
        print(f"        pass --ga to point elsewhere.", file=sys.stderr)
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
                print(f"[warn] Cycle {cycle.index}: no generations, skipping",
                      file=sys.stderr)
                n_skipped += 1
                continue

            # Pick the final generation (highest index, which is also the
            # last appended).
            final_gen = max(cycle.generations, key=lambda g: g.index)

            try:
                row, lik = best_individual(final_gen)
            except (KeyError, ValueError) as exc:
                print(f"[warn] Cycle {cycle.index}: {exc}", file=sys.stderr)
                n_skipped += 1
                continue

            # Anchor the buffer start at cycle 0's t_now unless overridden.
            if buffer_start is None:
                buffer_start = cycle.t_now

            out_row: list[object] = [
                cycle.index,
                cycle.timestamp,
                f"{cycle.t_now:.6f}",
                f"{lik:.6e}",
            ]
            for label, _ in ACTIVE_CHANNELS:
                try:
                    mse, r2, nse = extract_channel_metrics(
                        final_gen.header, row, label
                    )
                except (KeyError, IndexError, ValueError) as exc:
                    print(f"[warn] Cycle {cycle.index} channel {label!r}: "
                          f"{exc}", file=sys.stderr)
                    mse = r2 = nse = float("nan")
                out_row += [f"{mse:.6e}", f"{r2:.6e}", f"{nse:.6e}"]

            # WLS effective sample size. All three active channels share
            # the same observation schedule (obs every obs_interval_hours
            # from buffer_start onward), so per-channel sum_w is identical;
            # total sum_w is n_channels * per-channel.
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

            out_row += [
                n_obs_pc,
                f"{sum_w_pc:.6e}",
                f"{sum_w_total:.6e}",
                f"{lik_per_w:.6e}",
            ]

            writer.writerow(out_row)
            n_cycles += 1

    print(f"Wrote {n_cycles} cycle rows to {args.out}"
          f" ({n_skipped} skipped)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
