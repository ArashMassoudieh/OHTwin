#!/usr/bin/env python3
"""
analyse_wetland_codegen_run1.py

Figures for the 2026-09-16 -> 2026-09-20 Wetland codegen run (241 MCMC cycles,
200x acceleration, 8 chains on 1 thread). Reads the deployment outputs directly;
pass --archive to read from the extracted backup instead.

Writes four PNGs at 240 dpi into --outdir.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Validated categorical slots (dataviz reference palette, light mode).
C_EST     = "#2a78d6"   # slot 1  blue    - estimate / modelled
C_TRUTH   = "#eb6834"   # slot 2  orange  - truth
C_THIRD   = "#1baf7a"   # slot 3  aqua
INK       = "#0b0b0b"
INK_2     = "#52514e"
GRID      = "#d9d8d4"

plt.rcParams.update({
    "font.size": 9,
    "axes.titlesize": 10,
    "axes.labelsize": 9,
    "axes.edgecolor": INK_2,
    "axes.labelcolor": INK,
    "text.color": INK,
    "xtick.color": INK_2,
    "ytick.color": INK_2,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.6,
    "legend.frameon": False,
})

# Known truth for the static parameters (from the truth model's .ohq).
TRUTH_STATIC = {
    "CatchmentRunoffCoeff": 0.75,
    "Evap_Coefficient": 1.0,
    "PondAlphaMultiplier": 1.0,
    "WetlandOutletAlpha": 100000.0,
}
SIGMA_INJECTED = {"Stage_Std": 0.005, "Outflow_Std": 20.0}


def load_param_history(path: Path):
    rows = list(csv.DictReader(open(path)))
    cyc = np.array([float(r["cycle"]) for r in rows])
    t = np.array([float(r["t_now"]) for r in rows])
    cols = {k: np.array([float(r[k]) for r in rows])
            for k in rows[0] if k not in ("cycle", "timestamp", "t_now")}
    return cyc, t, cols


def load_posterior(path: Path):
    recs = [json.loads(l) for l in open(path) if l.strip()]
    names = recs[-1]["parameter_names"]
    idx = {n: i for i, n in enumerate(names)}
    out = {
        "cycle": np.array([r["cycle"] for r in recs], float),
        "t_now": np.array([r["t_now"] for r in recs], float),
        "ess": np.array([r.get("ess") or 0.0 for r in recs], float),
        "sweeps": np.array([r.get("sweeps") or 0 for r in recs], float),
        "evals": np.array([r.get("evaluations") or 0 for r in recs], float),
        "converged": np.array([bool(r.get("converged")) for r in recs]),
    }
    for band in ("p025", "p975", "mean"):
        out[band] = {n: np.array([r[band][idx[n]] if r.get(band) else np.nan
                                  for r in recs], float) for n in names}
    return out


def load_drift(path: Path):
    ts, vs = [], []
    for r in csv.reader(open(path)):
        if len(r) >= 2:
            try:
                ts.append(float(r[0])); vs.append(float(r[1]))
            except ValueError:
                pass
    return np.array(ts), np.array(vs)


def drift_at(dt, dv, t):
    return dv[min(bisect.bisect_left(list(dt), t), len(dv) - 1)]


def load_series(path: Path):
    """selected_output.csv is interleaved (t, value) column pairs."""
    rows = list(csv.reader(open(path)))
    hdr = [h.strip() for h in rows[0]]
    out = {}
    for i in range(0, len(hdr) - 1, 2):
        name = hdr[i + 1]
        tv = [(float(r[i]), float(r[i + 1]))
              for r in rows[1:] if len(r) > i + 1 and r[i].strip()]
        if tv:
            out[name] = (np.array([a for a, _ in tv]), np.array([b for _, b in tv]))
    return out


def load_budget(cfg_path: Path):
    """(sweep cap, per-cycle wall-clock budget in seconds) from config.json."""
    c = json.load(open(cfg_path))
    asm, rt = c.get("assimilation", {}), c.get("runtime", {})
    cap = float(asm.get("mcmc_max_sweeps", 300))

    def secs(v, default):
        if v is None:
            return default
        m = re.match(r"\s*([0-9.]+)\s*([a-z]*)", str(v))
        if not m:
            return default
        x, unit = float(m.group(1)), m.group(2)
        return x * {"day": 86400, "hr": 3600, "min": 60, "s": 1, "": 1}.get(unit, 1)

    poll = secs(asm.get("poll_interval"), 3 * 86400)
    margin = secs(asm.get("mcmc_budget_margin"), 60)
    accel = float(rt.get("time_acceleration", 1.0))
    return cap, poll / accel - margin


def load_elapsed(path: Path):
    cyc, sec = [], []
    for r in csv.reader(open(path)):
        if len(r) > 5 and r[1] == "assim_calibration":
            try:
                cyc.append(int(r[2])); sec.append(float(r[5]))
            except ValueError:
                pass
    return np.array(cyc, float), np.array(sec, float)


# ---------------------------------------------------------------- figures ---

def fig_parameters(post, cyc, t, cols, dt, dv, out: Path, title_note=""):
    """Small multiples: each estimated parameter against its known truth."""
    panels = ["CatchmentRunoffCoeff", "PondAlphaMultiplier", "WetlandOutletAlpha",
              "Evap_Coefficient", "Soil_Hydraulic_Conductivity", "Stage_Std"]
    fig, axes = plt.subplots(3, 2, figsize=(11, 8.2), sharex=True,
                             constrained_layout=True)
    for ax, name in zip(axes.ravel(), panels):
        est = cols[name]
        lo, hi = post["p025"].get(name), post["p975"].get(name)
        n = min(len(cyc), len(lo))
        ax.fill_between(cyc[:n], lo[:n], hi[:n], color=C_EST, alpha=0.20, lw=0,
                        label="95% credible")
        ax.plot(cyc, est, color=C_EST, lw=1.6, label="estimate")

        if name in TRUTH_STATIC:
            ax.axhline(TRUTH_STATIC[name], color=C_TRUTH, lw=2.0, ls="--",
                       label="truth")
        elif name == "Soil_Hydraulic_Conductivity":
            tv = np.array([drift_at(dt, dv, x) for x in t])
            ax.plot(cyc, tv, color=C_TRUTH, lw=2.0, ls="--", label="truth (drifting)")
            ax.set_yscale("log")
        elif name == "Stage_Std":
            ax.axhline(SIGMA_INJECTED[name], color=C_TRUTH, lw=2.0, ls="--",
                       label="injected noise")
        ax.set_title(name, loc="left", color=INK)
        ax.margins(x=0.01)
    for ax in axes[-1]:
        ax.set_xlabel("calibration cycle")
    axes[0, 0].legend(loc="lower right", fontsize=8)
    fig.suptitle(f"Parameter recovery over {len(cyc)} cycles  —  "
                 f"estimate vs known truth{title_note}",
                 fontsize=12, x=0.01, ha="left")
    fig.savefig(out, dpi=240, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] {out}")


def fig_drift_coupling(cyc, t, cols, dt, dv, out: Path):
    """The headline science result, and the ET coupling that explains it.

    Two stacked panels sharing the x axis - never a second y scale on one plot.
    """
    ksat = cols["Soil_Hydraulic_Conductivity"]
    evap = cols["Evap_Coefficient"]
    truth = np.array([drift_at(dt, dv, x) for x in t])

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8.4), sharex=True,
                                        constrained_layout=True)

    ax1.plot(cyc, truth, color=C_TRUTH, lw=2.2, ls="--", label="truth (20x clogging)")
    ax1.plot(cyc, ksat, color=C_EST, lw=1.8, label="estimate")
    ax1.set_yscale("log")
    ax1.set_ylabel("Ksat (m/day)")
    late = slice(len(cyc) // 2, None)
    end_ratio = float(np.median(ksat[late] / truth[late]))
    ax1.set_title(f"Soil_Hydraulic_Conductivity — drift tracked; second-half "
                  f"estimate runs {end_ratio:.1f}x truth", loc="left")
    ax1.legend(loc="lower left", fontsize=8)

    ax2.axhline(1.0, color=C_TRUTH, lw=2.0, ls="--", label="truth = 1.0")
    ax2.plot(cyc, evap, color=C_THIRD, lw=1.8, label="estimate")
    ax2.set_yscale("log")
    ax2.set_ylabel("Evap_Coefficient")
    ev_late = float(np.median(evap[late]))
    ev_min, ev_max = float(np.min(evap[late])), float(np.max(evap[late]))
    ax2.set_title(f"Evap_Coefficient — second-half median {ev_late:.2f} "
                  f"(range {ev_min:.2f}-{ev_max:.2f}) against truth 1.0",
                  loc="left")
    ax2.legend(loc="lower left", fontsize=8)

    ratio = ksat / truth
    ax3.axhline(1.0, color=INK_2, lw=1.0)
    ax3.plot(cyc, ratio, color=C_EST, lw=1.8, label="Ksat estimate / truth")
    ax3.set_yscale("log")
    # Clip to the informative band; cycles 1-20 are burn-in and one spike to
    # 3e-3 would otherwise compress the 1-10x range the panel is about.
    ax3.set_ylim(0.5, 20)
    ax3.set_ylabel("over-estimate factor")
    ax3.set_xlabel("calibration cycle")
    ax3.set_title(f"Ksat over-estimate tracks the ET shortfall — "
                  f"median {end_ratio:.1f}x over the second half", loc="left")
    ax3.legend(loc="upper left", fontsize=8)
    for ax in (ax1, ax2, ax3):
        ax.margins(x=0.01)
    fig.savefig(out, dpi=240, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] {out}")


def fig_sampling(post, elc, els, cap, budget, out: Path):
    """Sampling economics: what binds, the cap or the clock?

    Every caption here is computed from the run being plotted -- these numbers
    differ by an order of magnitude between the 1-thread and 8-thread runs.
    """
    import statistics as _st
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8.0), sharex=True,
                                        constrained_layout=True)
    c = post["cycle"]
    n = len(c)
    hits = int(np.sum(post["sweeps"] >= cap))
    med_ess = float(np.median(post["ess"]))
    over = int(np.sum(els >= budget)) if len(els) else 0

    ax1.axhline(cap, color=C_TRUTH, lw=2.0, ls="--",
                label=f"mcmc_max_sweeps = {cap:g}")
    ax1.plot(c, post["sweeps"], color=C_EST, lw=1.6, label="sweeps achieved")
    ax1.set_ylabel("sweeps / cycle")
    ax1.set_title(f"Sweep cap reached on {hits} of {n} cycles "
                  f"({100*hits/max(n,1):.0f}%)", loc="left")
    ax1.legend(loc="upper right", fontsize=8)

    ax2.axhline(budget, color=C_TRUTH, lw=2.0, ls="--",
                label=f"budget = {budget:.0f} s")
    ax2.plot(elc, els, color=C_EST, lw=1.6, label="elapsed")
    ax2.set_ylabel("seconds / cycle")
    ax2.set_title(f"Wall-clock deadline reached on {over} of {len(els)} cycles",
                  loc="left")
    ax2.legend(loc="lower right", fontsize=8)

    ax3.plot(post["cycle"], post["ess"], color=C_THIRD, lw=1.6, label="ESS")
    ax3.set_ylabel("effective sample size")
    ax3.set_xlabel("calibration cycle")
    ax3.set_title(f"Effective sample size — median {med_ess:.1f}", loc="left")
    ax3.legend(loc="upper right", fontsize=8)
    for ax in (ax1, ax2, ax3):
        ax.margins(x=0.01)
    fig.savefig(out, dpi=240, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] {out}")


def fig_validation(assim, truth, out: Path):
    """The state-handling fix: depths track truth, and never reach zero."""
    panels = [("Wetland inlet stage (m)", "inlet stage (m)"),
              ("Wetland Cell 6 Water depth (m)", "Cell 6 depth (m)"),
              ("Wetland outflow (m3/day)", "outflow (m3/day)")]
    fig, axes = plt.subplots(3, 1, figsize=(11, 8.0), sharex=True,
                             constrained_layout=True)
    for ax, (key, lab) in zip(axes, panels):
        if key not in assim or key not in truth:
            continue
        ta, va = assim[key]
        tt, vt = truth[key]
        ax.plot(tt, vt, color=C_TRUTH, lw=1.4, label="truth")
        ax.plot(ta, va, color=C_EST, lw=1.0, alpha=0.85, label="assimilation")
        ax.set_ylabel(lab)
        ax.margins(x=0.01)
    dk = "Wetland Cell 1 Water depth (m)"
    if dk in assim:
        v = assim[dk][1]
        nz, n = int(np.sum(v < 0.01)), len(v)
        note = (f"no zero-depth episodes (0 of {n:,} points)" if nz == 0
                else f"{nz:,} of {n:,} points below 0.01 m")
    else:
        note = ""
    axes[0].set_title(f"Forward model vs truth over the full record — {note}",
                      loc="left")
    axes[0].legend(loc="upper right", fontsize=8, ncol=2)
    axes[-1].set_xlabel("time (OHQ serial day)")
    fig.savefig(out, dpi=240, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="deployments",
                    help="directory holding the two deployment folders")
    ap.add_argument("--suffix", default="",
                    help="deployment folder suffix, e.g. '_mt' for the "
                         "8-thread run (default: the original folders)")
    ap.add_argument("--label", default="",
                    help="short run label appended to figure titles")
    ap.add_argument("--outdir", default="deployments/Plots/wetland_run1")
    a = ap.parse_args()

    root = Path(a.root)
    A = root / f"Wetland_assimilation_codegen{a.suffix}"
    T = root / f"Wetland_truth_codegen{a.suffix}"
    for p_ in (A, T):
        if not p_.is_dir():
            raise SystemExit(f"no such deployment: {p_}")
    note = f"   ({a.label})" if a.label else ""
    out = Path(a.outdir); out.mkdir(parents=True, exist_ok=True)

    cyc, t, cols = load_param_history(A / "outputs/calibration/parameter_history.csv")
    post = load_posterior(A / "outputs/calibration/posterior_history.jsonl")
    dt, dv = load_drift(T / "drift/ksat_drift_wetland.csv")
    elc, els = load_elapsed(A / "outputs/run_log.csv")
    assim = load_series(A / "outputs/selected_output.csv")
    truth = load_series(T / "outputs/selected_output.csv")

    cap, budget = load_budget(A / "config.json")
    fig_parameters(post, cyc, t, cols, dt, dv,
                   out / "01_parameter_recovery.png", note)
    fig_drift_coupling(cyc, t, cols, dt, dv, out / "02_drift_and_et_coupling.png")
    fig_sampling(post, elc, els, cap, budget, out / "03_sampling_economics.png")
    fig_validation(assim, truth, out / "04_forward_vs_truth.png")


if __name__ == "__main__":
    main()
