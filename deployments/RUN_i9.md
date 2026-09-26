# Wetland codegen run 3 — instructions for the i9 box

Run 3 changes **sampling only**. The model physics is byte-identical to runs 1
and 2, so the three are directly comparable.

| | run 1 | run 2 | run 3 (this) |
|---|---|---|---|
| chains | 8 | 8 | **16** |
| MCMC `number_of_threads` | 1 | 8 | **16** |
| solver `n_threads` | 8 | 1 | 1 |
| `mcmc_max_sweeps` | 300 | 300 | **900** |
| time_acceleration | 200 | 200 | 200 |
| ports (truth / assim) | 8090 / 8186 | 8091 / 8187 | **8092 / 8188** |

Everything else — window, 60-day calibration window, 3-day poll, priors,
observations, drift series — is unchanged.

## 1. What the i9 needs

Both repos, as **siblings** — this is now required at *runtime*, not just to
build. As of `2194e9c`, `OpenHydroTwin/resources` is a **symlink** to
`../OpenHydroQual/resources`, and the twin resolves templates through it:

```
<parent>/OpenHydroQual/      # core sources + the real resources/ templates
<parent>/OpenHydroTwin/      # the twin + deployments; resources -> ../OpenHydroQual/resources
```

Check it landed: `ls -l resources` must show the symlink, and
`ls resources/settings.json` must succeed. If you copy rather than clone, use
`cp -a` or `tar` — plain `cp -r` dereferences the symlink and silently
duplicates ~20 k lines of templates.

Build dependencies (Ubuntu/Debian names):

```bash
sudo apt install build-essential qmake6 qt6-base-dev qt6-base-dev-tools \
                 libarmadillo-dev liblapack-dev libblas-dev libgsl-dev \
                 libomp-dev nginx
```

## 2. Build

```bash
cd <parent>/OpenHydroTwin && mkdir -p build-qmake && cd build-qmake \
  && qmake6 ../OHTwin.pro && make -j$(nproc)
```

Produces `build-qmake/bin/OHTwin`. Expect warnings; there should be no errors.
If the link fails on duplicate `Composite` symbols, the `.pro` fix (commit
7f5b9e6) is missing — pull it.

## 3. nginx

The assimilator reads truth over HTTP, so nginx must serve both output folders.
Edit the paths in `deployments/wetland_codegen_i9.nginx` if the repo is not at
`/home/arash/Projects/OpenHydroTwin`, then:

```bash
sudo cp deployments/wetland_codegen_i9.nginx /etc/nginx/sites-available/wetland_codegen_i9
sudo ln -sf /etc/nginx/sites-available/wetland_codegen_i9 /etc/nginx/sites-enabled/wetland_codegen_i9
sudo nginx -t && sudo systemctl reload nginx
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8092/outputs/   # 200 or 403, not 404
```

nginx runs as `www-data` and needs execute (`+x`) on every directory down to
the repo. If 8092 returns 403, that is the cause.

The config also exposes `/viewer/`, but those page folders are **not** shipped
(they are ~16 MB of wasm each). `/outputs/` works without them; `/viewer/`
simply 404s. To enable the browser viewer on the i9, clone the run-2 pages and
repoint the ports:

```bash
cd <parent>/OpenHydroTwin
for n in truth assimilation; do
  cp -r viewer_pages/wetland_${n}_codegen_mt viewer_pages/wetland_${n}_codegen_i9
  sed -i 's/:8091/:8092/g; s/:8187/:8188/g' viewer_pages/wetland_${n}_codegen_i9/config.json
done
```

Then `http://localhost:8092/viewer/` and `http://localhost:8188/viewer/`.

## 4. Run — truth first, always

```bash
cd <parent>/OpenHydroTwin
./build-qmake/bin/OHTwin --deployment deployments/Wetland_truth_codegen_i9 --fresh --force
```

Wait until `deployments/Wetland_truth_codegen_i9/outputs/selected_output.csv`
exists and is growing (~1 min), then in a second terminal:

```bash
cd <parent>/OpenHydroTwin
./build-qmake/bin/OHTwin --deployment deployments/Wetland_assimilation_codegen_i9 --fresh --force
```

`--fresh` is REQUIRED: the resume anchors are past `stop_datetime`, so without
it both exit immediately having done nothing. Starting the assimilator against
an already-finished truth makes it attempt one enormous catch-up solve, which
is why truth goes first and only a little ahead.

## 5. Check on the first calibration cycle (~21 min in)

```
runCycle: cycle=1 chains=16 threads=16
```

If it says `threads=1`, the model edit did not take — check line 17 of
`model/Wetland.ohq`.

```
[Assim] weather injected over ... : precip N bins, T ..., R_h ..., wind ..., rad ...
```

All five counts must be non-zero. A zero precip count now aborts the cycle
loudly rather than silently solving an unforced window.

## 6. Expected

~3.8 days wall-clock (731 daily cycles at 200x, 432 s per cycle — the pace is
set by the clock, not the CPU, so a faster box does not finish sooner; it
samples more per cycle).

Per cycle, extrapolating from run 2 (8 chains / 8 threads gave 2312 evals and
288 sweeps): **roughly 2x the evaluations**, since 16 chains do 16 evals per
sweep at about the same wall-clock cost per sweep. Sweeps per cycle will stay
near run 2's unless the deadline allows more — the raised 900 cap only helps
the ~36% of cycles that were cap-bound rather than deadline-bound.

The honest expectation: the gain here is mostly **ensemble quality** (16 chains,
plateau quorum 0.25 x 16 = 4) rather than raw throughput. If `Evap_Coefficient`
still wanders across a factor of ~3 at double the samples, that is good evidence
the problem is identifiability, not compute — which is the point of the run.

## 7. Bringing results back

```bash
cd <parent>/OpenHydroTwin/deployments
tar czf wetland_run3_i9.tar.gz Wetland_truth_codegen_i9 Wetland_assimilation_codegen_i9
```

Then regenerate the figures anywhere:

```bash
python3 deployments/Plots/analyse_wetland_codegen_run1.py \
        --suffix _i9 --label "16 chains / 16 threads" \
        --outdir deployments/Plots/wetland_run3
```

Every caption in those figures is computed from the run being plotted, so they
are safe to regenerate against any of the three runs.
