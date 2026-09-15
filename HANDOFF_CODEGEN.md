# HANDOFF — running an assimilation on a codegen kernel

**Written 2026-09-14.** Companion to `CODEGEN_ROADMAP.md` (the plan) and
`OpenHydroQual/issues.md` (the defect log). This file is the practical one: what
works today, how to run it, and what is still missing before the *twin* can use
a kernel.

---

## 1. TL;DR — where we actually are

The model compiler is **done and validated**. Roadmap gaps G1–G9 are closed; the
generated kernel is numerically faithful and 60–1000× faster than the
interpreter on every deployment model tested.

**But:** the twin binary (`OHTwin`) has **no kernel wiring at all**. Nothing in
`/home/arash/Projects/OpenHydroTwin` mentions `solver_backend`,
`codegen_library` or `KernelSystem` — verified by grep. So:

| you want to… | can you today? |
|---|---|
| batch-calibrate a model with GA or MCMC on a kernel | **yes** — `OHQ-GA` / `OHQ-MCMC --kernel` |
| run the streaming digital twin (`OHTwin --deployment …`) on a kernel | **no** — Phase 2 is unwritten (§6) |

So "perform the assimilation later" splits into two very different jobs. §5 is
the one that works now; §6 is the work remaining for the twin proper.

---

## 2. What is built and proven

### The compiler
`OpenHydroQual/codegen/` — `ohq_generate <model.ohq> <resources> <out> <Class>
[stateVar] [--project exe|lib|shared]`.

Emits a self-contained C++ class plus (with `--project`) an embedded copy of the
header-only runtime, a `CMakeLists.txt`, and either a driver `main.cpp` or a C
API. No Qt, no armadillo, no OHQ core in the generated code.

### Capabilities (roadmap ids)
- **G1** parameters are live: `setParameter(i,v)` + `applyParameters()`, index =
  the model's parameter order (what `SetParameterValue(i,·)` uses).
- **G2** observations recorded every accepted step; `uniformizeObservations()`
  reproduces `System::FinalizeOutputs`.
- **G4** forcing injectable by name: `setSeries(object, quantity, t[], v[], n)`
  and `setPrecipitation(...)` (bins → midpoint intensities, as the interpreter).
- **G5** state values in/out: `exportState/importState` — **values only**; model
  structure stays with `System`.
- **G6** status: `solutionFailed()`, `simulationDuration()`, `stepCount()`.
- **G7** copy-safe: a copied kernel re-binds to itself (`bindSelf()`), so
  `chains[k] = base` is correct *and* compiles.
- **G3/G9** host side: `ohq::KernelSystem` (below).

### The host integration (G3 + G9)
`OpenHydroQual/terminal/OHQ-Common/ohq_kernel.h` — `ohq::KernelSystem` derives
from `System` and **shadows `Solve()`**. Because `CGA<T>`/`CMCMC<T>` dispatch
statically, instantiating `CGA<KernelSystem>` swaps *only* the solve; parameters,
`ApplyParameters`, `CalcMisfit`, outputs and the GA/MCMC drivers are untouched.
It copies the kernel's observation series into
`observation(i)->SetModeledTimeSeries()`, so `GetObjectiveFunctionValue()` scores
exactly what it would after an interpreter solve — **identical posteriors, no
misfit code reimplemented** (G3). Likelihood parameters (an observation's
`error_standard_deviation`) are applied by `System::ApplyParameters` as usual and
simply ignored by the kernel (G9).

### The ABI
`OpenHydroQual/codegen/tools/ohq_kernel.h` — **ABI v2**, 40 fixed `ohq_kernel_*`
entry points that every generated library exports, plus an `ohq::Kernel` dlopen
loader with a version guard. Needs no OHQ core, so a host that does not link
`System` (the twin's `DTRunner`, §6) can use it directly.

### Measured coverage (2026-09-14)
All 15 deployment models generate. Parity = max relative error on final storages
vs the interpreter, both driven the way MCMC drives them:

| model | size | parity | speed-up |
|---|---|---|---|
| Reservoir | 2 blk | 4.55e-16 | 545× |
| Bioretention | 11 | 4.99e-10 | 443× |
| **Wetland (truth & assimilation codegen)** | 10 / 13 lnk | **1.75e-06** | **699×** |
| R_simple | 36 | 8.27e-11 | 318× |
| JM | 26 | 5.57e-06 | 1082× |
| R_LF | 192 | 1.27e-11 (1% perturbation) | 397× |
| StormwaterPond | 5 | 1.14e-03 *(loosest)* | 61× |
| 8-column study | 152 blk, 1368 masses | −0.05% breakthrough | 52× |
| R / HQ / VN | 532 / 391 / 450 blk | compile only | — |

Wetland with two years of real hourly forcing: **0.13% / 0.2% RMS**, 31×
(`deployments/Wetland_truth_codegen/benchmark/`).

---

## 3. The two codegen deployments — READ BEFORE RUNNING

`deployments/Wetland_truth_codegen/` and `deployments/Wetland_assimilation_codegen/`
were copied from the originals (existing deployments untouched, as instructed).

Both **generate, compile and pass parity**:

| | truth_codegen | assimilation_codegen |
|---|---|---|
| blocks / links | 10 / 13, 0 build errors | 10 / 13, 0 build errors |
| parameters | 5 | 7 (5 physical + `Stage_Std`, `Outflow_Std`) |
| observations | 10 | 10 |
| forcing series | 34 | 34 |
| compile | OK | OK |
| parity | PASS, 1.75e-06 | PASS, 1.75e-06 (identical) |

The two trajectories are identical because the extra parameters are the G9
sigmas — they move the likelihood, not the physics. Confirmed in the header:

```
Stage_Std   -> Wetland inlet stage (m).error_standard_deviation  : applied by the host
Stage_Std   -> Wetland outlet stage (m).error_standard_deviation : applied by the host
Outflow_Std -> Wetland outflow (m3/day).error_standard_deviation : applied by the host
```

### ⚠ Blocker: name and port still collide with the originals
Neither `config.json` was re-pointed. Today they read:

| deployment | `deployment.name` | `deployment.port` |
|---|---|---|
| `Wetland_truth_codegen` | `Wetland_truth` ← collides | **8088** ← collides |
| `Wetland_assimilation_codegen` | `Wetland_assimilation_MCMC` ← collides | **8184** ← collides |

Starting either alongside the originals will fight over the port and the systemd
instance name. Ports already in use across deployments: 8081–8089, 8091–8094,
8182–8185, 8194. **Free and adjacent: 8090 and 8186.** Also update
`assimilation.truth_csv_url` in the assimilation config to point at the new
truth port, or it will assimilate against the *original* truth twin.

---

## 4. Build everything from scratch

```bash
OHQ=/home/arash/Projects/OpenHydroQual

# 1. the generator (needs Qt6 + libOHQLib)
cmake -S $OHQ/codegen -B $OHQ/codegen/build -DCMAKE_BUILD_TYPE=Release
cmake --build $OHQ/codegen/build -j8          # -> ohq_generate, test_emitter, kernel_abi_test

# 2. the GA / MCMC runners (qmake6, NOT qmake)
cd $OHQ/terminal/OHQ-GA   && qmake6 OHQ-GA.pro   && make -j8
cd $OHQ/terminal/OHQ-MCMC && qmake6 OHQ-MCMC.pro && make -j8
```

---

## 5. Run an assimilation on a kernel — the path that works today

```bash
OHQ=/home/arash/Projects/OpenHydroQual
DEP=/home/arash/Projects/OpenHydroTwin/deployments/Wetland_assimilation_codegen

# 1. generate a SHARED library from the very model you will calibrate
cd $DEP/model                                   # run from the model's folder (§7.1)
$OHQ/codegen/build/ohq_generate Wetland.ohq $OHQ/resources ./kernel Wetland Storage --project shared

# 2. build it
cmake -S ./kernel -B ./kernel/build -DCMAKE_BUILD_TYPE=Release
cmake --build ./kernel/build -j8                # -> kernel/build/libWetland.so

# 3. sanity-check the kernel on its own (no model knowledge needed)
$OHQ/codegen/build/kernel_abi_test ./kernel/build/libWetland.so

# 4. calibrate
$OHQ/terminal/OHQ-MCMC/OHQ-MCMC Wetland.ohq . --kernel ./kernel/build/libWetland.so
# or:  $OHQ/terminal/OHQ-GA/OHQ-GA Wetland.ohq . --kernel ./kernel/build/libWetland.so
```

Startup prints a verification you should actually read:

```
Forward model  : generated kernel ./kernel/build/libWetland.so
Kernel class    : Wetland
Kernel verified : 7 parameters, 10 observations, names match the model;
                  all 5 kernel-owned parameters move it (host-owned skipped).
```

That last clause is the **parameter self-test**: it perturbs every kernel-owned
parameter and refuses to start if one is advertised but ignored. It exists
because a GA once "converged" on an optimum that did not reproduce, having
optimised parameters the kernel had frozen as literals (issues.md ISSUE 17).
Name/count matching alone would not have caught it.

**Drop `--kernel` to fall back to the interpreter.** Same binary, same outputs,
same objective — that is the comparison to run whenever a result looks odd.

### The model needs a real simulation window
Deployment `.ohq` files are templates: the twin injects the window and the
weather per cycle. `Wetland_assimilation_codegen/model/Wetland.ohq` carries
`simulation_start_time = 40178.8`, `simulation_end_time = 40542.8`, and **empty
forcing** — a 364-day drawdown with no rain. For a real calibration, either

- inject forcing through the kernel (`ohq_kernel_set_precipitation` /
  `_set_series`, G4), or
- point the source at a file, as `benchmark/Wetland_forward.ohq` does (see
  `benchmark/README.md` for the Open-Meteo fetch and the exact unit conventions:
  precipitation as `start,end,depth[m]` bins; RH as a **fraction**, not %).

---

## 6. What is missing for the TWIN (Phase 2)

`OHTwin` still solves with the interpreter, always. To give it the option:

1. **`DTConfig`**: add `assimilation.solver_backend: "interpreter" | "codegen"`
   and `assimilation.codegen_library: "<path>.so"`. Default **interpreter**.
2. **`DTAssimilation`**: where it builds the calibration `System`
   (`prepareCalibrationSystem`, ~`:777`), construct an `ohq::KernelSystem`
   instead when the backend is `codegen`, `LoadKernel()` + `VerifyKernelMatches()`
   once at startup, and instantiate `CGA<KernelSystem>` / the streaming MCMC
   against it. `DTStreamingMCMC` derives from `CMCMC<System>`, so it needs
   templating on `T` (or a second instantiation) — **this is the one genuinely
   invasive change**; everything else is additive.
3. **Forcing + restart through the kernel**: the twin injects weather every cycle
   and hot-restarts from a snapshot. Map
   `DTRunner::injectPrecipitation` → `ohq_kernel_set_precipitation`,
   `DTWeather::injectWeather` → `ohq_kernel_set_series`, and the snapshot's block
   values → `ohq_kernel_import_state` / `_export_state`. The ABI already exposes
   all of these (that is why it was widened to 40 symbols); nothing in the
   compiler should need to change.
4. **Staleness guard**: a kernel is compiled from one `.ohq`. If the deployment's
   model changes, the kernel is silently wrong. `VerifyKernelMatches` checks
   parameter and observation names/counts; consider also recording the model
   hash at generation time and refusing on mismatch.

A cheaper first step, if you want the speed without the invasive change: keep the
twin on the interpreter for the forward loop, and use `OHQ-MCMC --kernel` (§5)
for the heavy calibration cycles offline.

---

## 7. Gotchas that will cost you an afternoon

**7.1 Run every OHQ tool from the model's folder.** `addtemplate` resolves a
template filename **relative to the process's working directory first**, and only
then in `resources/` (`Command.cpp:207`). A stale `mass_transfer.json` in the
repo root once silently replaced the real template and produced hours of bogus
"parity" numbers (issues.md ISSUE 6).

**7.2 The console needs `-w .` when the model's output paths are relative.**
`OpenHydroQual-Console attrib/m.ohq` sets the working folder to `attrib/`, so a
model writing `attrib/out.txt` lands in `attrib/attrib/` — and if that directory
does not exist, a 200 s run produces **nothing**, silently.

**7.3 Regenerate kernels after any generator change.** The ABI is **v2**; a
library built before 2026-09-14 reports v1 and is refused with
`kernel ABI v1, host expects v2`. That guard exists because the ABI was once
widened without a version bump (issues.md ISSUE 20).

**7.4 Compare against the interpreter only with `dt` capped.** The interpreter's
`dt` clamp registers **precipitation series only**, so on dry days it samples
hourly ET inputs twice a day and its ET series is not a valid reference
(issues.md ISSUE 8). Set `max_timestep_increase_factor ≈ 2` on both sides when
checking parity. The kernel clamps on all forcing series.

**7.5 A "PASS" over a run that never advanced means nothing.** `parity_obs` now
reports `INCONCLUSIVE` when `tend <= tstart` or the interpreter failed, because
two models reported glowing passes with 0 steps (issues.md ISSUE 21).

**7.6 Two deployment models are broken** (pre-existing, not codegen bugs):
- `Wetland_truth/model/Wetland_BSh_AM.ohq` loads **7 of its 13 links**. It has
  template blocks from two machines, and the second `loadtemplate` *resets* the
  template set while omitting `groundwater.json`, so `surface2groundwater_link`
  is undefined and `Soil_Hydraulic_Conductivity` calibrates nothing.
- `HQ.ohq` declares `Ks_12` and `new_Van_alpha` with no `setasparameter` binding.
- `Wetland_sensitivity_analysis/Wetland_SA.ohq` generates but reports **11 build
  errors — never investigated.**

The generator now prints a model's own build errors before generating; read them.

**7.7 `maximum_simulation_time` is the kernel's only escape hatch.**
`KernelSystem::Solve` steps rather than calling `run_to` so it can check the
clock: `run_to` has no cancellation, and a parameter set that collapses `dt` to
the floor runs effectively forever — which is how an MCMC with 16 chains once
stalled on its first sample.

---

## 8. Verifying nothing has rotted

```bash
OHQ=/home/arash/Projects/OpenHydroQual
B=/home/arash/Projects/OpenHydroTwin/deployments/Wetland_truth_codegen/benchmark

$OHQ/codegen/build/test_emitter                     # expression emitter
$OHQ/codegen/tests/run_parity_obs.sh $B/Wetland_obs_60d_dtfix.ohq WObs 0.15   # G1+G2, expect PASS
$OHQ/codegen/build/kernel_abi_test <some>/libX.so   # ABI v2 + parameter self-test
python3 $B/compare_outputs.py $B/results_dtfix      # the 2-year forcing benchmark
```

`tests/kernel_api_test.cpp` covers G4/G5 (needs the generated header, not the .so).

---

## 9. File map

| path | what |
|---|---|
| `OpenHydroQual/codegen/` | the compiler: `src/CodeGenerator.cpp`, `runtime/` (header-only), `tools/ohq_kernel.h` (ABI v2), `tests/` |
| `OpenHydroQual/terminal/OHQ-Common/ohq_kernel.h` | `KernelSystem` — the G3/G9 host hook |
| `OpenHydroQual/terminal/OHQ-GA`, `OHQ-MCMC` | the `--kernel` runners |
| `OpenHydroQual/issues.md` | **21 issues**; the codegen/interpreter defect log |
| `OpenHydroTwin/CODEGEN_ROADMAP.md` | the plan and the G1–G10 gap table |
| `deployments/Wetland_truth_codegen/benchmark/` | the real-forcing benchmark + `compare_outputs.py` + its own README |

### Uncommitted at handoff time (OpenHydroQual)
`codegen/src/CodeGenerator.cpp`, `codegen/tests/parity_obs.cpp`, `issues.md` —
the G8 work (§2 coverage, the `pressure_head` fix, the INCONCLUSIVE guard).
Everything else is committed and pushed (`c3ddff8`).

---

## 10. Open items

| item | where |
|---|---|
| **Twin wiring (Phase 2)** — the only thing between here and a kernel-backed twin | §6 |
| Assign fresh name/port to the two codegen deployments | §3 |
| `Wetland_SA.ohq`'s 11 build errors | §7.6 |
| **G10**: interpreter accepts negative storage; kernel mirrors the policy but takes a different Newton path, so drainage-terminus blocks can differ ~2% | issues.md ISSUE 1 — *deferred by you* |
| ISSUE 8 (dt clamp ignores non-precipitation forcing) is an **interpreter** bug affecting deployed ET | issues.md ISSUE 8 |
| Symbolic/AD Jacobian, dead-code elimination | issues.md ISSUE 3 |
