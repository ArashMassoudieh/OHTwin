#!/usr/bin/env python3
"""
make_posterior_distributions.py

Build per-parameter posterior distributions from the cumulative pooled-sample
dump (outputs/calibration/posterior_samples.csv) over a chosen cycle window,
and write them to a single CSV with a PAIR of columns per parameter:

    <param>_value, <param>_density, <param2>_value, <param2>_density, ...

Each parameter has its own independent value grid (parameters live on different
scales), so the value columns differ column-to-column; row index is just the
grid point number and carries no cross-parameter meaning.

Why a cycle window: this is a STREAMING filter -- each cycle's posterior targets
a different sliding data window, so naively pooling ALL cycles mixes distinct
targets. Restrict to a stationary stretch (e.g. after the point estimates in
parameter_ci_history.csv have settled) for a clean, publication-ready marginal.

Usage
-----
    python make_posterior_distributions.py \\
        --input  outputs/calibration/posterior_samples.csv \\
        --cycle-min 150 --cycle-max 300 \\
        --output posterior_distributions.csv \\
        [--grid 200] [--method kde|hist] [--bins 60] \\
        [--converged-only] [--pad 0.03]

Density estimate defaults to a Gaussian KDE (smooth, good for figures); use
--method hist for a normalized histogram. KDE uses scipy if available, else a
NumPy fallback. Densities integrate to ~1 over each parameter's range.
"""

import argparse
import csv
import sys
import numpy as np

META_COLS = ("cycle", "t_now", "converged")


def read_samples(path, cycle_min, cycle_max, converged_only):
    """Read posterior_samples.csv, return (param_names, {name: np.array of values})."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            sys.exit(f"error: {path} is empty")

        header = [h.strip() for h in header]
        # Map column indices.
        try:
            i_cycle = header.index("cycle")
            i_conv = header.index("converged")
        except ValueError:
            sys.exit("error: input must have 'cycle' and 'converged' columns "
                     "(is this a posterior_samples.csv?)")
        param_names = [h for h in header if h not in META_COLS]
        param_idx = [header.index(p) for p in param_names]

        cols = {p: [] for p in param_names}
        n_rows = 0
        n_kept = 0
        for row in reader:
            if not row:
                continue
            n_rows += 1
            try:
                cyc = int(float(row[i_cycle]))
            except (ValueError, IndexError):
                continue
            if cycle_min is not None and cyc < cycle_min:
                continue
            if cycle_max is not None and cyc > cycle_max:
                continue
            if converged_only:
                try:
                    if int(float(row[i_conv])) != 1:
                        continue
                except (ValueError, IndexError):
                    continue
            for p, j in zip(param_names, param_idx):
                try:
                    cols[p].append(float(row[j]))
                except (ValueError, IndexError):
                    cols[p].append(np.nan)
            n_kept += 1

    arrays = {p: np.asarray(v, dtype=float) for p, v in cols.items()}
    return param_names, arrays, n_rows, n_kept


def gaussian_kde_eval(values, grid):
    """KDE on `grid`. Prefer scipy; fall back to a NumPy Silverman-bandwidth KDE."""
    try:
        from scipy.stats import gaussian_kde
        return gaussian_kde(values)(grid)
    except Exception:
        # NumPy fallback: Silverman's rule of thumb bandwidth.
        n = values.size
        std = np.std(values, ddof=1)
        if std == 0 or n < 2:
            return np.zeros_like(grid)
        iqr = np.subtract(*np.percentile(values, [75, 25]))
        sigma = min(std, iqr / 1.349) if iqr > 0 else std
        h = 0.9 * sigma * n ** (-1 / 5)
        if h <= 0:
            return np.zeros_like(grid)
        # (grid x n) Gaussian kernels, averaged.
        u = (grid[:, None] - values[None, :]) / h
        k = np.exp(-0.5 * u * u) / np.sqrt(2 * np.pi)
        return k.mean(axis=1) / h


def distribution_for(values, method, grid_n, bins, pad):
    """Return (value_grid, density) for one parameter."""
    v = values[np.isfinite(values)]
    if v.size == 0:
        return np.array([]), np.array([])

    lo, hi = float(v.min()), float(v.max())
    if hi <= lo:
        # Degenerate (constant) parameter: a single spike.
        return np.array([lo]), np.array([1.0])

    span = hi - lo
    lo_p, hi_p = lo - pad * span, hi + pad * span

    if method == "hist":
        density, edges = np.histogram(v, bins=bins, range=(lo_p, hi_p),
                                      density=True)
        centers = 0.5 * (edges[:-1] + edges[1:])
        return centers, density

    grid = np.linspace(lo_p, hi_p, grid_n)
    return grid, gaussian_kde_eval(v, grid)


def main():
    ap = argparse.ArgumentParser(
        description="Per-parameter posterior distributions over a cycle window.")
    ap.add_argument("--input", default="outputs/calibration/posterior_samples.csv",
                    help="cumulative pooled-sample CSV (default: %(default)s)")
    ap.add_argument("--output", default="posterior_distributions.csv",
                    help="output CSV (default: %(default)s)")
    ap.add_argument("--cycle-min", type=int, default=None,
                    help="lowest cycle to include (inclusive)")
    ap.add_argument("--cycle-max", type=int, default=None,
                    help="highest cycle to include (inclusive)")
    ap.add_argument("--method", choices=("kde", "hist"), default="kde",
                    help="density estimate (default: kde)")
    ap.add_argument("--grid", type=int, default=200,
                    help="KDE evaluation points per parameter (default: 200)")
    ap.add_argument("--bins", type=int, default=60,
                    help="histogram bins when --method hist (default: 60)")
    ap.add_argument("--pad", type=float, default=0.03,
                    help="fractional range padding on each side (default: 0.03)")
    ap.add_argument("--converged-only", action="store_true",
                    help="use only rows with converged=1")
    args = ap.parse_args()

    names, arrays, n_rows, n_kept = read_samples(
        args.input, args.cycle_min, args.cycle_max, args.converged_only)

    if n_kept == 0:
        sys.exit(f"error: no samples in the selected window "
                 f"(cycle_min={args.cycle_min}, cycle_max={args.cycle_max}, "
                 f"converged_only={args.converged_only}); read {n_rows} rows total")

    win = []
    if args.cycle_min is not None:
        win.append(f">= {args.cycle_min}")
    if args.cycle_max is not None:
        win.append(f"<= {args.cycle_max}")
    win_str = " and ".join(win) if win else "all cycles"
    print(f"[dist] {n_kept} samples ({win_str}"
          f"{', converged only' if args.converged_only else ''}) "
          f"across {len(names)} parameters; method={args.method}")

    # Build each parameter's (value, density) pair.
    per_param = {}
    max_len = 0
    for p in names:
        grid, dens = distribution_for(
            arrays[p], args.method, args.grid, args.bins, args.pad)
        per_param[p] = (grid, dens)
        max_len = max(max_len, grid.size)
        print(f"[dist]   {p}: n={np.isfinite(arrays[p]).sum()} "
              f"range=[{arrays[p][np.isfinite(arrays[p])].min():.6g}, "
              f"{arrays[p][np.isfinite(arrays[p])].max():.6g}] rows={grid.size}")

    # Assemble the paired-column CSV. Grids may differ in length across
    # parameters (equal for KDE, equal for hist); pad short ones with blanks.
    header = []
    for p in names:
        header += [f"{p}_value", f"{p}_density"]

    with open(args.output, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for r in range(max_len):
            row = []
            for p in names:
                grid, dens = per_param[p]
                if r < grid.size:
                    row += [f"{grid[r]:.8g}", f"{dens[r]:.8g}"]
                else:
                    row += ["", ""]
            w.writerow(row)

    print(f"[dist] wrote {args.output} "
          f"({max_len} rows x {2 * len(names)} columns)")


if __name__ == "__main__":
    main()
