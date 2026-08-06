#!/usr/bin/env python3
"""Progress check for the inflation + drift-detection test run.

Compares the live run against the completed control
(Bioretention_assimilation_MCMC_drift). The question this answers is narrow:
does the pooled ensemble spread STABILISE, or keep contracting as it did in the
control (377x over 244 cycles)?

Usage:  python3 deployments/Bioretention_assimilation_MCMC_drift_INFLATE/check.py
"""
import csv, glob, json, os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEPL = os.path.dirname(HERE)
TEST = os.path.join(DEPL, "Bioretention_assimilation_MCMC_drift_INFLATE")
CTRL = os.path.join(DEPL, "Bioretention_assimilation_MCMC_drift")
DRIFT_ONSET, DRIFT_END = 44166.0, 44197.0

NAMES = ("EngineeredSoilAlpha EngineeredSoilKsat EngineeredSoiln NativeSoilKsat "
         "NativeSoiln RunoffCoeff Std_PondWaterDepth Std_SoilMoisture "
         "Std_UnderdrainFlow").split()
ISLOG = [n != "EngineeredSoiln" for n in NAMES]


def history(root):
    p = os.path.join(root, "outputs/calibration/posterior_history.jsonl")
    if not os.path.exists(p):
        return []
    out = []
    for line in open(p):
        line = line.strip()
        if line:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass          # tolerate a torn final line while running
    return out


def pool_sd(root):
    """Mean per-parameter sd of each cycle's pool, in proposal space."""
    p = os.path.join(root, "outputs/calibration/posterior_samples.csv")
    if not os.path.exists(p):
        return {}
    byc = {}
    with open(p) as fh:
        rows = csv.reader(fh)
        next(rows, None)
        for r in rows:
            if len(r) < 4:
                continue
            try:
                byc.setdefault(int(r[0]), []).append([float(x) for x in r[3:]])
            except ValueError:
                pass
    out = {}
    for c, v in byc.items():
        A = np.array(v)
        if len(A) < 2:
            continue
        Z = A.copy()
        for i, lg in enumerate(ISLOG):
            if lg and i < A.shape[1]:
                Z[:, i] = np.log(np.maximum(A[:, i], 1e-300))
        out[c] = float(np.nanmean(Z.std(0, ddof=1)))
    return out


def sigma_hat(root):
    """Accumulated Sigma-hat mean sd, by cycle, from the archived files."""
    out = {}
    for f in glob.glob(os.path.join(root, "outputs/proposal_cov_cycle_*.txt")):
        t = open(f).read()
        h = re.search(r"cycle=(\d+).*?kappa=([\d.eE+-]+)", t, re.S)
        m = re.search(r"^sd,([-\d.eE+,]+)$", t, re.M)
        if h and m:
            sd = np.array([float(x) for x in m.group(1).split(",")])
            out[int(h.group(1))] = (sd.mean(), float(h.group(2)))
    return out


def block_table(H, S, tag):
    print(f"\n--- {tag}: pool spread by block (the collapse diagnostic) ---")
    print(f"{'cycles':<12}{'pool sd':<12}{'accept':<9}{'ess':<8}{'plateau':<9}{'conv':<7}")
    cyc = sorted(c for c in S)
    if not cyc:
        print("  (no pooled samples yet)")
        return
    byc = {h["cycle"]: h for h in H}
    step = max(len(cyc) // 6, 1)
    for a in range(0, len(cyc), step):
        blk = [c for c in cyc[a:a + step]]
        sd = np.median([S[c] for c in blk])
        hs = [byc[c] for c in blk if c in byc]
        if not hs:
            continue
        print(f"{f'{blk[0]}-{blk[-1]}':<12}{sd:<12.6f}"
              f"{np.median([x['acceptance_rate'] for x in hs]):<9.3f}"
              f"{np.median([x['ess'] for x in hs]):<8.1f}"
              f"{np.median([x['plateaued_fraction'] for x in hs]):<9.2f}"
              f"{sum(1 for x in hs if x['converged'])}/{len(hs)}")


def main():
    Ht, St = history(TEST), pool_sd(TEST)
    if not Ht:
        print("No cycles yet in the INFLATE run — has it started?")
        return 1
    Hc, Sc = history(CTRL), pool_sd(CTRL)

    last = Ht[-1]
    print(f"INFLATE run: {len(Ht)} cycles, t_now {last['t_now']:.0f} "
          f"(drift {DRIFT_ONSET:.0f}-{DRIFT_END:.0f})")
    print(f"  seed_inflation reported: {last.get('seed_inflation', 'ABSENT')}"
          f"   kappa {last.get('kappa', float('nan')):.6g}"
          f"   floor {last.get('kappa_floor', float('nan')):.6g}"
          f"{'   <-- SATURATED' if last.get('kappa', 1) <= last.get('kappa_floor', 0) * 1.001 else ''}")

    block_table(Ht, St, "INFLATE")
    if Hc:
        block_table(Hc, Sc, "CONTROL (completed, no inflation)")

    # headline: is spread holding?
    cyc = sorted(St)
    if len(cyc) >= 20:
        early = np.median([St[c] for c in cyc[:10]])
        late = np.median([St[c] for c in cyc[-10:]])
        print(f"\n--- VERDICT ---")
        print(f"  pool spread first10 -> last10 : {early:.5f} -> {late:.5f} "
              f"(x{early / late:.1f} contraction)" if late > 0 else "  degenerate")
        per = (late / early) ** (1.0 / max(len(cyc) - 10, 1))
        print(f"  per-cycle factor              : {per:.4f}  "
              f"({'HOLDING - inflation working' if per >= 0.999 else 'STILL COLLAPSING - try r=1.10'})")
        print(f"  control per-cycle factor      : 0.9760 (for reference)")

    # Sigma-hat staleness
    G = sigma_hat(TEST)
    if G:
        print(f"\n--- Sigma-hat vs pool (control ran 200-1000x; ~1-5x is healthy) ---")
        print(f"{'cycle':<8}{'kappa':<12}{'sd_hat':<11}{'sd_pool':<11}{'ratio':<8}")
        for c in sorted(G)[-6:]:
            sh, k = G[c]
            sp = St.get(c)
            r = f"{sh / sp:.0f}" if sp and sp > 0 else "n/a"
            print(f"{c:<8}{k:<12.6g}{sh:<11.5f}{(sp if sp else float('nan')):<11.6f}{r:<8}")

    # drift detector
    dd = [h for h in Ht if "cusum_max" in h]
    if dd:
        print(f"\n--- drift detector ---")
        ref = last.get("drift_tau")
        print(f"  autocorr time tau : {ref:.1f} cycles" if ref else "  tau: n/a")
        alarms = [h for h in dd if h.get("drift_detected")]
        pre = [h for h in alarms if h["t_now"] < DRIFT_ONSET]
        post = [h for h in alarms if h["t_now"] >= DRIFT_ONSET]
        print(f"  cusum_max (latest): {last.get('cusum_max', float('nan')):.2f}  (alarm at h=5.0)")
        p = last.get("drift_t2_p", -1)
        print(f"  T2 p-value        : {'insufficient history' if p < 0 else f'{p:.3e}'}")
        print(f"  FALSE alarms (pre-drift) : {len(pre)}"
              f"{'  <-- detector too sensitive' if pre else '  (good)'}")
        if post:
            print(f"  first TRUE alarm  : t={post[0]['t_now']:.0f} "
                  f"(lag {post[0]['t_now'] - DRIFT_ONSET:+.0f} d; baseline CUSUM was +19 d)")
        elif last["t_now"] > DRIFT_END:
            print("  no alarm yet despite drift completing  <-- detector too insensitive")
    else:
        print("\n--- drift detector: no records yet (needs mcmc_drift_detection: true) ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
