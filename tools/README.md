# tools

## `repro_cycle.cpp` — reproduce a failing twin cycle locally

The twin logs solver failures as `state_variable: 0`, which is the *flow
phase*, not a block — it never names the block that actually diverged. The
engine can name it, via `System::SetSolutionLogger()`, but that is a C++ API
with no config switch, so the deployments do not enable it.

This driver loads the exact state a failing cycle used
(`<deployment>/state/_current_input.json`), applies the same window, turns the
solution logger on, and solves. The log then reports, per failure:

    Maximum error location:
      - JM Street Gutter 4

Build (adjust paths for your checkout):

```bash
OHQ=../OpenHydroQual; QT=/usr/include/x86_64-linux-gnu/qt6
g++ -O1 -std=c++17 -fPIC -Dubuntu_version -DTerminal_version -DQ_JSON_SUPPORT \
    -DARMA_USE_LAPACK -DARMA_USE_BLAS -DGSL \
    -I$OHQ/aquifolium/include -I$OHQ/aquifolium/src -I$OHQ/aquifolium/include/GA \
    -I$OHQ/aquifolium/include/MCMC -I$OHQ/jsoncpp/include -I$OHQ -I$QT -I$QT/QtCore \
    tools/repro_cycle.cpp -o repro_cycle \
    -L$OHQ/OHQLib/build/rel-sys -lOHQLib -Wl,-rpath,$OHQ/OHQLib/build/rel-sys \
    -larmadillo -llapack -lblas -lgsl -lgomp -lQt6Core -lpthread
```

Run:

```bash
./repro_cycle <input.json> <tstart> <tend> <solution.log>
```

Then:

```bash
grep -A2 "Maximum error location:" solution.log | grep '^  - ' | sort | uniq -c | sort -rn
```

Two gotchas it already handles:

- **The simulation window must be written with `SetSettingsParameter()`.**
  `SetSystemSettings()` re-pushes the whole Settings vector over the System
  properties, so a bare `SetProp()` before it is silently reverted to the
  snapshot's own window.
- **Forcing series are not in the snapshot.** `SavetoJson` does not serialize
  time-series data, so a rain-driven failure needs the weather injected
  separately. A dry-down failure (precipitation zero) reproduces from the
  snapshot alone.
