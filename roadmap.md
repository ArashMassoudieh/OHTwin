# Roadmap — running OpenHydroTwin's assimilation on a codegen-generated forward model

**Goal.** Let a deployment's GA / streaming-MCMC data assimilation use a
*compiled, model-specific C++ library* (produced by OpenHydroQual's model
compiler, `OpenHydroQual/codegen/`) as the forward model instead of the
expression interpreter — as a **config option**, with the interpreter kept as
the default and fallback, and with **the same likelihood** so posteriors are
unchanged (only faster).

**Why it pays.** Every assimilation cycle runs thousands of forward solves
(`DTStreamingMCMC::posteriorLocal`, `CMCMC::posterior`, `CGA::assignfitnesses`),
and each one re-walks every `Expression` tree in every Newton iteration. On the
AZ12-140 grid the generated solver is ~16× faster than the interpreter
(62.9 s vs 1000.8 s); on small deployment models the per-solve overhead of
copying a whole `System` per sample dominates and the gain is larger still.

Status date: 2026-09-13. Written after reading both code bases; every seam
below names the actual function.

---

## 1. What the twin actually asks of the forward model

Both estimators are **templates on the model type** and touch the model
through one narrow contract per evaluation:

| step | `CMCMC<T>::posterior` (`MCMC.hpp:198`) / `DTStreamingMCMC::posteriorLocal` (`DTStreamingMCMC.cpp:2146`) / `CGA<T>::assignfitnesses` (`GA.hpp:334`) |
|---|---|
| 1 | `T work = *Model;` (or `m_chainModels[c]`) — **copy the pristine model** |
| 2 | `SetSilent(true); SetRecordResults(false); SetNumThreads(1)` |
| 3 | `SetParameterValue(i, par[i])` for each parameter (+ log-prior from `Parameter`) |
| 4 | `ApplyParameters()` → `object(loc)->SetVal(quan, value)` for each `(location, quan)` of each `Parameter` (`System.cpp:3721`), then `composites[i].Propagate()` |
| 5 | `Solve()` — during the solve `UpdateObservations(t)` calls `observations[i].append_value(t)` **every accepted step** (`System.cpp:3479`), evaluating each observation's `expression` on its `object` |
| 6 | `GetObjectiveFunctionValue()` → `+1e18` if `GetSolutionFailed()`, else `CalcMisfit()` = Σ `Observation::CalcMisfit()` (`observation.cpp:110`): `modeled_time_series` vs `observed_data` under the observation's `comparison_method` / `error_structure` / kernel weights |
| 7 | `GetSolutionFailed()`, `GetSimulationDuration()` for bookkeeping |

Around that loop the twin does three more things the kernel must support:

- **Hot restart from a snapshot.** Cycles start from a *full model JSON*
  (`SavetoJson` / `LoadfromJson`: `Blocks`, `Links`, `Parameters`,
  `Observations`, `Sources`, `Set As Parameters`, `Settings`, `_dt_*` times) —
  `DTRunner.cpp:601-665`, `DTAssimilation::prepareCalibrationSystem`
  (`:777`), `buildSpinupSnapshot` (`:669`, one `Solve()` to spin up).
- **Runtime forcing injection.** Weather is pushed into the loaded model:
  `DTRunner::injectPrecipitation` → `system->source("Rain")->Variable("timeseries")->SetTimeSeries(precip)`;
  `DTAssimilation::injectCalibrationWeather` likewise; `applyParameterDrift`
  writes `Parameter::SetValue` from a drift series.
- **Parallel chains.** `#pragma omp parallel for` over chains
  (`DTStreamingMCMC.cpp:1255,1363`), each with its own `System` copy
  (`m_chainModels`). The kernel must be re-entrant with no shared mutable state.

Two facts that shape the design:

1. `DTConfig` already carries the knobs (`assimilation.method = "GA"|"MCMC"`,
   `scriptFile`, `loadModelJson`, `calibrationOutputDir`, …) — a
   `solver_backend` option slots in naturally.
2. `OHTwin.pro` builds with `OHQ_FROM_SOURCE = 1`, i.e. the twin **compiles the
   OHQ core itself**. So it can also link `codegen/` and *generate* a kernel for
   the loaded model on the fly, not only load a pre-built one.

---

## 2. Gap analysis — what a generated model must provide that it does not today

Today the generated class (`<Class>.h` from `CodeGenerator::generate`) exposes
`initialize / step / stepTo / runTo / time / state(i) / stateName(i)` and, for
constituents, `constMass(i)`. Parameters are **constant-folded**, observations
are **not emitted**, forcing series are **baked** at generation time, and there
is no state import/export. Each gap below is a concrete codegen work item.

| id | gap | what to build | where |
|---|---|---|---|
| **G1** ✅ 2026-09-13 | Parameters are folded into literals | Treat every quantity bound by `setasparameter` (`Parameter::GetLocations()/GetQuans()`) as a **runtime input**: emit `double p_<obj>_<quan>` members, keep them out of the Constant tier, and emit `applyParameters(const double* v)` that rebuilds every constant that depends on them (the analyzer already has the dependency graph — add a "parameter" root). Parameter → (object,quan) map emitted as a table so the twin can address by *parameter index*, matching `SetParameterValue(i, …)`. | `DependencyAnalyzer`, `CodeGenerator` |
| **G2** ✅ 2026-09-13 | Observations are not emitted | For each `Observation` emit its `expression` evaluated on `object` as a per-step function `observe(t, out[])`, using the same expression pipeline (they are ordinary quantity expressions: `depth`, `flow`, `AgeTracker_1:concentration`, `(Precipitation*1000)`, `(0-Evapotranspiration)`). The runtime records them after every accepted step into per-observation `(t, value)` series, exactly like `UpdateObservations`. | `CodeGenerator`, `ohq_massbalance.h` (post-step hook) |
| **G3** ✅ 2026-09-14 | Misfit / likelihood | **Do not reimplement.** Hand the recorded series back to the interpreter's `Observation::modeled_time_series` and let `CalcMisfit()` run unchanged (all comparison methods, log-normal, kernel weights, KS, autocorrelation). This guarantees identical posteriors. (A native port of `TimeSeries::diff2/weighted_mse/…` is the Strategy-B follow-up.) | OHQ core hook (§3) |
| **G4** ✅ 2026-09-13 | Forcing baked at gen time | Public setters for every baked source/time series (`setSeries("Rain", t[], v[], n)`), refreshing the dt **breakpoints** used to clamp steps. Needed for `injectPrecipitation` and ET inputs. The existing `set_ts_*` setters cover object series; extend to shared `ts_src_*` sources. | `CodeGenerator`, `ohq_timeseries.h` |
| **G5** ✅ 2026-09-13 | No state import/export | `exportState(storage[], mass[], limited[], t)` / `importState(...)` — **state variable values only** (storages, constituent masses, outflow-limited flags, link flows, time). The kernel never reads or writes model *structure*: the full-model JSON snapshot (`SavetoJson`/`LoadfromJson`) stays with `System`, which hands the values across. | `ohq_massbalance.h`, `ohq_transport.h` |
| **G6** ✅ 2026-09-14 | Solver status | `solutionFailed()`, `simulationDuration()`, iteration counters — what `GetObjectiveFunctionValue` and the MCMC detail log read. Already partly there (`lastIterations`); add the failure flag and wall time. | runtime |
| **G7** ✅ 2026-09-14 | Copy semantics | The kernel is POD arrays; copying it per chain/sample is trivial. Make the generated class trivially copyable (no raw pointers) so `T work = m_chainModels[c]` stays cheap once the kernel replaces the heavy `System` copy. | `CodeGenerator` |
| **G8** ✅ 2026-09-14 | Model coverage for deployed models | **Wetland: DONE 2026-09-13** — (a) Penman ET source internal graph emitted as source-context member functions; (b) flow-phase geometry cached for the transport phase; (c) sources inside constituent expressions expanded; plus transport-dt and `/`,`^` parity fixes found on the way (see `deployments/Wetland_truth_codegen/benchmark/README.md`: 5–7× faster, 0.13% / 0.2% RMS agreement). Still to survey: Bioretention / Drywell / Reservoir deployments for soil/Green-Ampt forms, composites (`Composite::Propagate` after `ApplyParameters`), multi-constituent Arrhenius reactions. | `CodeGenerator` |
| **G9** ✅ 2026-09-14 | Parameters that are *not* model quantities | `setasparameter` can bind an observation's `error_standard_deviation` (`Stage_Std`, `Outflow_Std` in Wetland). These live in the likelihood, not the physics — the kernel must skip them and the hook must still apply them to the interpreter-side `Observation`. | hook |
| **G10** | Interpreter parity edge cases | `OpenHydroQual/issues.md` ISSUE 1: the interpreter accepts negative storage in a converged step; the generated limiter mirrors the policy but Newton paths differ, so drainage-terminus blocks can diverge ~2%. Decide per model whether that matters for the likelihood (Wetland stages/flows are far from that regime). | both |

---

## 3. Integration architecture — two strategies, phased

### Strategy A — "solver swap" inside `System::Solve` (recommended first)

Keep `System` as the model type `T`. Add to the OHQ core an optional,
abstract **solver kernel** that `Solve()` delegates the time stepping to when
one is attached; everything else — parameters, `ApplyParameters`, observations,
`CalcMisfit`, GA/MCMC, snapshots, the twin's threads and plumbing — is
**untouched**.

```
System::Solve(applyparameters, ...)
  if (kernel_) {
     kernel_->applyParameters(paramValues)          // G1  (from Parameters(), skipping G9 ones)
     kernel_->setSeries(...)                        // G4  (only if a source/series is dirty)
     kernel_->importState(blocks, constituents, t)  // G5  (current block values -> kernel)
     ok = kernel_->run(tstart, tend, observeCb)     // G2  observeCb appends to observations[i]
     kernel_->exportState(...)                      // G5  kernel -> blocks (for snapshots/outputs)
     SolverTempVars.SolutionFailed = !ok;           // G6
     return ok;
  }
  ... existing interpreter path ...
```

- **ABI.** A small versioned C header, `ohq_kernel_api.h` (`OHQ_KERNEL_ABI = 1`),
  with a struct of function pointers (`create/destroy/n_params/param_name/
  apply_params/set_series/import_state/export_state/run/observation_count/
  observation_series/status`). The generated library (Export to C++ → *library*)
  implements it; the twin `dlopen`s / `LoadLibrary`s it. A C ABI keeps the
  kernel compiler-independent (MSVC on Windows, GCC on the servers).
- **Loading.** `config.json` → `assimilation.solver_backend: "interpreter" | "codegen"`,
  `assimilation.codegen_library: "<path>.so|.dll"`. The library records the
  **SHA-256 of the `.ohq`/snapshot structure it was generated from**; the twin
  refuses (and logs) a stale kernel and falls back to the interpreter.
- **Blast radius.** One hook in `System.cpp`, one config block in `DTConfig`, one
  loader in `DTAssimilation`/`DTRunner`. No change to `MCMC.hpp`, `GA.hpp`,
  `DTStreamingMCMC`.
- **Cost that remains.** `T work = m_chainModels[c]` still copies a `System`
  (heavy: maps of `Quan`, expression trees). Fine for phase 1; removed in
  Strategy B.

### Strategy B — a native drop-in model type (later)

A `CodegenModel` class satisfying the §1 contract *natively* (`SetParameterValue`,
`ApplyParameters`, `Solve`, `GetObjectiveFunctionValue`, `GetSolutionFailed`, …)
so the twin instantiates `CMCMC<CodegenModel>` / `CGA<CodegenModel>` directly.
Removes the `System` copy per evaluation and the interpreter from the hot loop
entirely, but needs: a native misfit (port of the `TimeSeries` misfit math),
prior/parameter plumbing, snapshot read/write against the kernel, and
templating `DTStreamingMCMC` on `T`. Do this once Strategy A has proven the
kernel numerically and the copy overhead is what is left on the profile.

### Generation modes

1. **Pre-built** (phase 1): the user runs *Model > Export to C++ → Library*
   (or `ohq_generate --project shared`) on the deployment's model, builds it
   with CMake (Linux/macOS/Windows-Visual Studio — already generated), and
   points `codegen_library` at it.
2. **Auto-generated at deployment start** (phase 4): because the twin links the
   OHQ core, it can call `ohqcg::CodeGenerator` on the *loaded* `System`
   (snapshot or `.ohq`), invoke `cmake --build`, and `dlopen` the result —
   regenerating whenever the model hash changes. Requires a compiler on the
   host; keeps the interpreter fallback.

---

## 4. Phases, deliverables, validation gates

### Phase 0 — done
- Model compiler: emitter, analyzer, generator, rules, outflow limiting,
  sources + breakpoint clamping, transport (advection + reactions), sparse
  colored-FD Jacobian; parity harness (`codegen/tests/`).
- **Export to C++** (GUI `Model > Export to C++…`, CLI `ohq_generate --project
  exe|lib|shared`): self-contained folder with generated header, embedded
  runtime, `CMakeLists.txt` (Windows/VS ready), `main.cpp` or C-API library.
  Verified: exe runs; static and shared libraries build and export a C ABI.

### Phase 1 — make the generated model *assimilable* (codegen only)
**G3 + G9 were already done** (2026-09-14, in `OpenHydroQual/terminal/OHQ-Common/ohq_kernel.h`,
commit `163a99f`): `ohq::KernelSystem` derives from `System` and *shadows*
`Solve()`, so instantiating `CGA<KernelSystem>` / `CMCMC<KernelSystem>` swaps
only the solve -- template dispatch is static and nothing in the library
changes. It pushes the parameter values into the kernel, steps it under the
model's `maximum_simulation_time` budget, calls `uniformize_observations` (=
`System::FinalizeOutputs`) and copies the kernel's series into
`observation(i)->SetModeledTimeSeries()`, after which `GetObjectiveFunctionValue()`
scores exactly what it would have after an interpreter solve -- **G3**, with no
misfit code reimplemented. **G9** falls out of the same design: `ApplyParameters()`
runs as usual, so a parameter bound to an observation's `error_standard_deviation`
reaches the interpreter-side `Observation`, and pushing it to the kernel as well
is harmless because the kernel stores a value it never reads. Wired up in
`OHQ-GA`/`OHQ-MCMC` behind `--kernel <lib.so>`.

Since 2026-09-14 that host also runs the **parameter self-test** at startup
(issues.md ISSUE 17 tasks C3/C4): names and counts matching is not enough, so
`VerifyKernelMatches` perturbs every kernel-owned parameter and refuses to
calibrate against one the kernel ignores.

**G8 done 2026-09-14.** All 15 deployment models generate; Bioretention,
Reservoir, StormwaterPond, JM, R_simple and R_LF match the interpreter to
5e-16..1e-3 on final storages (61-1082x faster), and the three large ones
(R 532 blocks, HQ 391, VN 450) compile. Fixed on the way: a bare quantity a link
does not own was emitted as an undeclared symbol (HQ/VN `pressure_head`) where
the interpreter scores it 0; a dangling capture in the resolver; and a parity
harness that passed runs which never advanced. Found two broken deployment
models -- `Wetland_BSh_AM.ohq` loads 7 of 13 links (duplicate template blocks,
the second `loadtemplate` resets and omits groundwater.json) and `HQ.ohq` has two
estimated parameters bound to nothing. Details in `OpenHydroQual/issues.md`
ISSUE 21.

**Kernel ABI done 2026-09-14.** `OpenHydroQual/codegen/tools/ohq_kernel.h`:
the 40 fixed `ohq_kernel_*` entry points every generated library exports, plus
an `ohq::Kernel` dlopen loader with an ABI-version guard, so ONE host binary can
drive any generated model chosen at run time. The alias ABI was extended to the
full capability set (it previously lacked G4 forcing injection, G5 state
in/out, and state/mass readback, which the twin needs). `Kernel::self_test()`
perturbs every advertised parameter and verifies the kernel responds — the
cheap startup check ISSUE 17 called for (tasks C3/C4). Test:
`codegen/tests/kernel_abi_test.cpp`, run against a built `.so`; validated on
Wetland and the 8-column model.

**Progress 2026-09-14:** **G6 + G7 done.**
G6 — `solutionFailed()` / `simulationDuration()` / `stepCount()` / `resetStatus()`
on the kernel and in both the C API and the `ohq_kernel_*` alias ABI; `step()`
latches the failure flag so `stepTo()` drivers see it, `runTo()` accumulates wall
time, and `initialize()` resets both (a new GA/MCMC sample).
G7 — turned out to be a **correctness** problem, not a cost one: the solvers held
`Model&` and the dt clamp held pointers into the model's own series, so a copied
kernel solved the ORIGINAL's parameters/forcing and the class was not even
copy-assignable (`chains[k] = base` did not compile). Fixed with a rebindable
`Model*` + a generated `bindSelf()` that `step()` calls if it detects it has been
copied. Copy-then-perturb is now bit-identical to a fresh kernel. Measured copy
cost is 121 us against a 0.25-2 s solve, so sharing the forcing is not worth it
(`OpenHydroQual/issues.md` ISSUE 19).

**Progress 2026-09-13:** G1 + G2 done and gated (`codegen/tests/run_parity_obs.sh`:
all parameters perturbed, both sides driven MCMC-style; on the 60-day Wetland
model with dt capped every observation agrees to ≤ 3e-3 rms, final storages
7e-5). Found on the way: the interpreter's dt clamp follows precipitation only,
so it samples hourly ET twice a day on dry days (`OpenHydroQual/issues.md`
ISSUE 8) — the kernel clamps on all forcing series instead.
**G4 + G5 done** (`codegen/tests/kernel_api_test.cpp`): `setSeries` /
`setPrecipitation(object, quantity, …)` by name — the same bins injected
through the API reproduce the baked trajectory bit-for-bit; `exportState` /
`importState(t, storage, mass, limited, limitFactor)` — values only — restart
within 1.6e-12 of a straight run. Both also in the C API. Remaining: G6, G7,
kernel ABI, then the `System::Solve` hook (Phase 2).
Deliverables: G1 parameters-as-inputs, G2 observation emission, G4 series
setters, G5 state import/export, G6 status, G7 copyable, `ohq_kernel_api.h` v1
implemented by the exported library.
**Gate:** on `Wetland.ohq` (forward model, MCMC scaffolding stripped) the
kernel's recorded observation series match the interpreter's
`modeled_time_series` to ≤1e-6 relative for every observation, for 3 random
parameter vectors; `applyParameters` round-trips (`state after apply` equals a
fresh generation with those values).
Prerequisite inside this phase: close **G8(a,b)** so Wetland generates and
compiles (Penman ET internal graph; flow-phase geometry in transport).

### Phase 2 — hook + twin integration (Strategy A)
Deliverables: `System::SetSolverKernel()` + `Solve()` delegation with the
observe callback into `observations[i]`; `DTConfig` `solver_backend` /
`codegen_library`; loader with hash check and fallback; `[Assim]` log lines
reporting backend and per-solve timing.
**Gate:** `deployments/Wetland_assimilation_MCMC` run end-to-end twice
(interpreter vs codegen) from the same snapshot and seed: identical
`GetObjectiveFunctionValue` per sample (≤1e-8 rel), identical acceptance
sequence, posterior mean/MAP identical; report speedup per cycle.

### Phase 3 — performance
Symbolic/AD Jacobian from the expression trees (replaces colored FD); dead-code
elimination (only quantities reachable from residual + observations);
per-chain kernel reuse so `posteriorLocal` copies only the kernel arrays
(wrap `System` copy: share the immutable parts); OpenMP scaling check across
chains; profile spin-up `Solve` and forward-loop steps too.
**Gate:** ≥10× cycle speedup on Wetland; no change in posteriors.

### Phase 4 — Strategy B + auto-generation
`CodegenModel` native model type; native misfit port with a parity test against
`Observation::CalcMisfit`; auto-generate/compile on model-hash change at
deployment start; Windows deployment path (MSVC kernel build) exercised.

### Cross-cutting
- **CI parity suite**: every deployment model in `deployments/*/model/*.ohq`
  generated + compared to the interpreter (`codegen/tests/run_parity.sh`).
- **Docs**: `codegen/README.md` (done for export), this roadmap, a
  `deployments/<name>/RUN.md` note on how to build and point at the kernel.

---

## 5. Risks and open questions

- **Interpreter validation errors on assimilation templates.** The console
  refuses `Wetland.ohq` with `Error: 0 was not found!` while the twin loads the
  same model from a snapshot JSON without complaint. The kernel must be
  generated from whatever object the twin actually solves (the loaded
  `System`), not from the `.ohq` text — Strategy A's hook gets this for free.
- **Composites.** `ApplyParameters` calls `Composite::Propagate` — group-level
  parameters reach members only through it; the codegen parameter table must
  be built *after* propagation (or emit the propagation).
- **Likelihood-side parameters (G9)** must keep flowing to `Observation`s even
  when the physics runs in the kernel.
- **Numerical determinism.** Different compilers/flags (MSVC vs GCC, FMA) can
  perturb the last bits; gates use tolerances, and MCMC acceptance sequences
  are compared only within one platform.
- **Parity edge cases (G10)** — negative-storage acceptance; fix in both per
  `issues.md` when it matters for a deployment.
- **Thread safety** — no `static` mutable state in generated code (the
  `static const` name tables are fine); one kernel instance per chain.

---

## 6. Immediate next steps (in order)

1. ~~Wetland forward-model executable~~ **done** — `deployments/Wetland_truth_codegen/benchmark/`
   (new deployment folders `Wetland_truth_codegen`, `Wetland_assimilation_codegen`
   created; existing ones untouched). Decision: **Strategy A**, interpreter stays
   the default, library is the second option; truth generator first.
2. Draft `codegen/runtime/ohq_kernel_api.h` (ABI v1) and add it to the
   library export.
3. G1 parameters-as-inputs + G2 observations in the generator; extend the
   parity harness to compare observation series and post-`applyParameters`
   state.
4. G4/G5/G6 in the runtime.
5. `System::SetSolverKernel` hook + `DTConfig` option + loader; Phase-2 gate on
   `Wetland_assimilation_MCMC`.
