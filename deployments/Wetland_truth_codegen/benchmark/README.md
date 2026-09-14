# Wetland forward-model benchmark — interpreter vs generated C++

Physics-only Wetland model (the deployment's `model/Wetland.ohq` without the
MCMC scaffolding: no observations / parameters / `setasparameter`), driven by
two years of real hourly forcing, solved by the OpenHydroQual interpreter and by
the C++ code generated from the same model. Purpose: measure the speedup and
verify the outputs agree before wiring a generated library into the twin.

Date: 2026-09-13. Nothing in the existing deployments was touched.

## Files

| file | what |
|---|---|
| `Wetland_forward.ohq` | physics-only model, `tstart=18264`, `tend=18995` (Unix-day serial, 2020-01-03 → 2022-01-03 — the twin's `toOHQDaySerial` convention), forcing wired to `forcing/` |
| `Wetland_forward_dtfix.ohq` | same, with `max_timestep_increase_factor=2` (dt ≤ 0.02 d) |
| `forcing/` | Open-Meteo archive, hourly, lat 38.91088 / lon −76.98569 (the deployment's site): `rain.txt` (CPrecipitation bins `start, end, depth[m]`), `temperature.txt` (°C), `rh.txt` (fraction — the twin divides % by 100), `wind.txt` (m/s), `solar.txt` (W/m²) |
| `codegen/`, `codegen_dtfix/` | generated projects (`ohq_generate --project exe`); `build/<Class>_solver` |
| `results/`, `results_dtfix/` | interpreter `output.txt`, generated CSV, logs, `comparison.md`, `comparison.png` |
| `compare_outputs.py` | interpolates the generated series onto the interpreter's times, tabulates errors, plots |

## How to reproduce

```bash
B=$PWD                                  # this folder
OHQ=/home/arash/Projects/OpenHydroQual
# interpreter (run FROM this folder: `addtemplate` resolves file names relative to the cwd first)
/usr/bin/time -f "wall=%e s" $OHQ/terminal/TOpenHydroQual/OpenHydroQual-Console \
    $B/Wetland_forward.ohq -r $OHQ/resources -o results/interp_output.txt > results/interp_run_verbose.log 2>&1
# generated (also from this folder, same reason)
$OHQ/codegen/build/ohq_generate $B/Wetland_forward.ohq $OHQ/resources $B/codegen WetlandForward --project exe
cmake -S codegen -B codegen/build -DCMAKE_BUILD_TYPE=Release && cmake --build codegen/build -j
/usr/bin/time -f "wall=%e s" codegen/build/WetlandForward_solver results/codegen_output.csv 2>&1 | tee results/codegen_run.log
python3 compare_outputs.py results
```

## Results

Both solvers integrate 731 days with hourly forcing (dt is clamped to every
series' sample times, so ≤ 30 min). The interpreter number includes writing its
73k-row × 426-column `output.txt`; the generated solver writes a 127k-row CSV.

| run | interpreter | generated | speedup | storages (rel RMSE / max at peaks) | tracer mass (rel RMSE / max) |
|---|---:|---:|---:|---|---|
| deployment settings (`max dt = 0.5 d`) | 30.95 s | 6.15 s | **5.0×** | 0.7% / 6–8% | 0.7–1.5% / ≤6.6% |
| dt ≤ 0.02 d (interpreter has no failed steps) | 44.57 s | 6.18 s | **7.2×** | **0.13% / ≤2.7%** (ponds) | **0.2% / ≤3%** |

Final values (t = 18995) agree to 0.02–0.3% in both runs; the boundary
reservoirs' tracer mass to 0.1%. See `results*/comparison.md` for the full
per-quantity table and `comparison.png` for the time series.

With the deployment settings the interpreter logs **365 "iterations exceeded"
events**, all in `state_variable: 1` (the transport solve) at dt = 0.5 d, and
cuts dt each time; the generated solver never fails. Capping dt removes those
events and the two trajectories converge (second row) — the residual peak
differences are step-path effects of two adaptive solvers, not model
differences.

## What had to be fixed to get here (all in `OpenHydroQual/codegen/`)

1. **Transport integrated over the wrong dt** — after an accepted flow step the
   runtime's `dt()` already holds the *grown* proposal for the next step
   (`×1/0.75`); the generated `step()` passed that to the transport phase, so
   tracer mass grew 33% too fast. Fixed to use `t_new − t_prev`. This was also the
   cause of the 2.7% `Decay_Test` gap logged earlier (now 2.9e-3).
2. **Division parity** — the interpreter evaluates every `/` as `a/(b+1e-23)`
   (`Expression::oprt`), so `0·0/0` is 0 there; C++ gave NaN on a link with no
   `area`/`length`. Emitted as `ohq::div`. Likewise `^` is `pow(max(a,0), b)`.
3. **Penman ET source** — its internal graph (`B, e_as, Ea, l_v, Er, Delta, rate`)
   is now emitted as member functions in the source's own context; a source
   without a `timeseries` contributes 1 for that factor.
4. **Flow-phase geometry in transport** — per-iteration flow quantities are cached
   as members after each accepted step (`computeFlowLocals`), so
   `diffusive_masstransfer` can read link `area`, and source coefficients can
   read `area`/`depth`.
5. **Sources inside constituent expressions** (`inflow_loading =
   c_in·(inflow+Precipitation) + Evapotranspiration·concentration`) are expanded
   in the transport context.
6. **Series baking** — 88k hourly points as one `push()` per line made GCC's
   optimizer run >10 min; now emitted as static tables (compile: 1.9 s).

## Two interpreter behaviours worth knowing (not changed)

- `addtemplate; filename = mass_transfer.json` resolves the file name **relative
  to the process's working directory first** (`Command.cpp:207`) and only then in
  the resources folder. A stale `mass_transfer.json` at the OpenHydroQual repo
  root silently replaced the real template when the generator was run from
  there (older AgeTracker: no ET term, different guard). Run tools from the
  model's folder.
- The console exits 4 on "property 'X' does not exist" for constituent
  expressions copied onto blocks lacking `X` (fixed_head: `Evapotranspiration`,
  `inflow`, `Precipitation`; Catchment link: `area`, `length`). They are treated
  as 0 and the run completes; the generator does the same.
