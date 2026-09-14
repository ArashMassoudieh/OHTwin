#!/usr/bin/env python3
"""
Compare the interpreter's output.txt with the codegen solver's CSV for the
Wetland forward benchmark, and summarize timing.

  interpreter : results/interp_output.txt   (OpenHydroQual-Console; columns come
                in "t, <name>" pairs, one time column per quantity)
  codegen     : results/codegen_output.csv  (one 'time' column, then
                <block>:Storage ... and <block>:<constituent>:mass ...)

Usage:  python3 compare_outputs.py [results_dir]
Writes results/comparison.md and results/comparison.png.
"""
import os
import sys
import numpy as np

R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")


def read_interp(path):
    with open(path) as f:
        hdr = [h.strip() for h in f.readline().rstrip("\n").split(",")]
        data = np.loadtxt(f, delimiter=",")
    # pairs: (t, name) -> {name: (t, v)}
    out = {}
    for i in range(0, len(hdr) - 1, 2):
        name = hdr[i + 1]
        out[name] = (data[:, i], data[:, i + 1])
    return out


def read_codegen(path):
    with open(path) as f:
        hdr = [h.strip() for h in f.readline().rstrip("\n").split(",")]
        data = np.loadtxt(f, delimiter=",")
    t = data[:, 0]
    return t, {hdr[i]: data[:, i] for i in range(1, len(hdr))}


def wall(logpath):
    try:
        for line in open(logpath):
            if "wall=" in line:
                return float(line.split("wall=")[1].split()[0])
    except OSError:
        pass
    return float("nan")


interp = read_interp(os.path.join(R, "interp_output.txt"))
tc, cg = read_codegen(os.path.join(R, "codegen_output.csv"))

rows = []
for cname, cv in cg.items():
    parts = cname.split(":")
    block = parts[0]
    iname = f"{block}_{':'.join(parts[1:])}"          # "Wetland Cell 1_Storage", "..._AgeTracker_1:mass"
    if iname not in interp:
        continue
    ti, vi = interp[iname]
    # common window, codegen interpolated onto interpreter times
    m = (ti >= tc[0]) & (ti <= tc[-1])
    ti, vi = ti[m], vi[m]
    vc = np.interp(ti, tc, cv)
    scale = max(np.max(np.abs(vi)), 1e-30)
    ae = np.abs(vi - vc)
    rows.append(dict(name=cname, final_i=vi[-1], final_c=vc[-1],
                     max_abs=ae.max(), max_rel=ae.max() / scale,
                     rmse_rel=np.sqrt(np.mean(ae ** 2)) / scale, scale=scale))

ti_wall = wall(os.path.join(R, "interp_run_verbose.log"))
cg_wall = wall(os.path.join(R, "codegen_run.log"))

lines = []
lines.append("# Wetland forward model — interpreter vs generated C++\n")
lines.append(f"- interpreter (OpenHydroQual-Console, writes output.txt): **{ti_wall:.2f} s** wall")
lines.append(f"- generated solver (WetlandForward_solver, writes CSV):   **{cg_wall:.3f} s** wall")
if cg_wall > 0:
    lines.append(f"- speedup: **{ti_wall / cg_wall:.1f}×**")
lines.append(f"- window: t = {tc[0]:.2f} → {tc[-1]:.2f} days ({tc[-1]-tc[0]:.0f} d); "
             f"interpreter rows {len(next(iter(interp.values()))[0])}, codegen rows {len(tc)}\n")
lines.append("| quantity | interp final | codegen final | max abs err | max rel err (scaled) | rel RMSE |")
lines.append("|---|---:|---:|---:|---:|---:|")
for r in rows:
    lines.append(f"| {r['name']} | {r['final_i']:.6g} | {r['final_c']:.6g} | {r['max_abs']:.3e} | {r['max_rel']:.3e} | {r['rmse_rel']:.3e} |")
worst = max(rows, key=lambda r: r["max_rel"]) if rows else None
if worst:
    lines.append(f"\nWorst scaled error: `{worst['name']}` max rel {worst['max_rel']:.3e} (scale {worst['scale']:.4g}).")
md = "\n".join(lines)
open(os.path.join(R, "comparison.md"), "w").write(md + "\n")
print(md)

# ---- plots ---------------------------------------------------------------
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    pick = [n for n in cg if n.endswith(":Storage") and "Wetland Cell" in n][:6]
    conc = [n for n in cg if n.endswith(":mass") and "Wetland Cell 6" in n]
    fig, axes = plt.subplots(len(pick) + len(conc), 1, figsize=(11, 2.2 * (len(pick) + len(conc))), sharex=True)
    for ax, n in zip(axes, pick + conc):
        parts = n.split(":"); iname = f"{parts[0]}_{':'.join(parts[1:])}"
        ti, vi = interp[iname]
        ax.plot(ti, vi, lw=1.2, label="interpreter")
        ax.plot(tc, cg[n], lw=0.8, ls="--", label="generated C++")
        ax.set_ylabel(n.replace("Wetland ", "").replace(":Storage", "\nStorage (m³)").replace(":AgeTracker_1:mass", "\nAge mass"), fontsize=8)
        ax.grid(alpha=.3)
    axes[0].legend(loc="upper right", fontsize=8)
    axes[-1].set_xlabel("time (days since 1970-01-01)")
    fig.suptitle("Wetland forward model: interpreter vs generated C++", fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(R, "comparison.png"), dpi=110)
    print("plot ->", os.path.join(R, "comparison.png"))
except Exception as e:  # matplotlib optional
    print("plot skipped:", e)
