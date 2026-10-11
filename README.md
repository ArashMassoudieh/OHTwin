# OHTwin
OHTwin is an open-source stormwater digital twin framework built around the OpenHydroQual simulation engine.
It provides a continuously running, sensor-coupled environment for real-time hydrologic simulation, state estimation, forecasting, visualization, and data assimilation of distributed stormwater infrastructure systems.

The framework was developed primarily for stormwater infiltration systems such as:

dry wells,
bioretention systems,
bioswales,
stormwater ponds,
engineered infiltration galleries,
and related green infrastructure assets.

OHTwin combines:

physically based hydrologic simulation,
online weather ingestion,
live observations,
data assimilation,
calibration/optimization,
state snapshots,
and browser-based visualization

into a unified digital twin architecture.

## OpenHydroQual resources

Clone `OHTwin` and `OpenHydroQual` as sibling directories. The `resources`
entry in OHTwin is a relative symlink to `../OpenHydroQual/resources`; the
build already uses source files from that sibling checkout. For example:

```sh
git clone https://github.com/ArashMassoudieh/OpenHydroQual.git
git clone https://github.com/ArashMassoudieh/OHTwin.git
```

Pull OpenHydroQual to use its latest resource templates locally:

```sh
git -C ../OpenHydroQual pull
```

Run this from the OHTwin directory. Check the OpenHydroQual changes before
deploying: `deploy.sh` and `deploy_jm.sh` copy the current JSON and list
templates to the server, so deployed copies update on the next deployment.
The former OHTwin-only `Pond_Plugin.json.bak_depth` is retained in
`resource_backups/`.

## Code-generated kernel backend and forcing map

A deployment can run its forward cycle with a model compiled to C++ by OpenHydroQual's code generator
(`ohq_generate model.ohq resources out Class Storage --project shared`) instead of the interpreter. The kernel
is loaded once at start-up through the model-independent `ohq_kernel_*` ABI
(`OpenHydroQual/codegen/tools/ohq_kernel.h`). Each cycle runs the usual Advance and Forecast stages: the kernel
starts from the latest state snapshot (state values by name, `state/state_*.json`), its forcing series are
replaced from the forcing map, and its observations feed `selected_output.csv` as before. A 3-year, 54-unit
watershed model advances one day in about a second.

```json
"solver": {
    "backend": "codegen",                       // default "interpreter"
    "library": "kernel/libMyModel.so",          // relative to the deployment
    "dt0": 0.0005,                              // initial time step of every stage (days)
    "parameters": { "p_Ksat": 3.2 }             // optional overrides by parameter name
},
"forcing": { "file": "forcing_map.json" }        // or the map inline
```

The forcing map gives one series per model source, for models with many forcing sources (one rainfall and
one ET source per sub-catchment):

```json
{ "past_days": 3, "forecast_days": 10,
  "entries": [
    { "name": "rain SC01", "variable": "precipitation", "provider": "openmeteo",
      "points": [ {"lat": 39.12, "lon": -77.11, "weight": 0.6}, {"lat": 39.13, "lon": -77.10, "weight": 0.4} ],
      "scale": 1.0, "targets": [ {"source": "P_SC01", "quantity": "timeseries"} ] },
    { "name": "ET0 SC01", "variable": "et0", "provider": "csv", "file": "forcing/et_sc01.csv",
      "targets": [ {"source": "ET_SC01", "quantity": "ET_timeseries"},
                   {"source": "ET2_SC01", "quantity": "ET_timeseries"} ] } ] }
```

- `variable`: `precipitation` (handed to the model as depth per interval) or `et0` (reference ET, m/day).
- `file` / `files` (csv provider): one file, or a list in which a later file replaces the earlier ones from its
  first time on (e.g. a long history file followed by a live feed's rolling window).
- `provider`: `openmeteo` (forecast API, hourly `precipitation` / `et0_fao_evapotranspiration`, all points of a
  cycle in batched multi-location requests, `past_days` back) or `csv` (an OpenHydroQual precipitation file
  `start,end,depth_m`, or a series `t,value`; times as OpenHydroQual day serials), for replays and for feeds
  prepared outside the twin.
- `scale`: multiplier, e.g. a bias factor to the forcing the model was calibrated with.
- `targets`: the model sources (and their time-series quantity) that receive the series.

With the codegen backend `deployment.viz_file` is optional (the SVG schematic needs an interpreter System).
Checked on the Rock Creek watershed model (547 states, 108 forcing entries): 60 daily cycles replaying the
calibration forcing reproduce a continuous run of the same kernel to 0.4 % (RMS of log flow).

The Forecast stage starts from the Advance end state (it does not repeat the Advance); `selected_output.csv` still
receives the Advance followed by the Forecast.

### Catch-up cycling and feeds

```json
"runtime": { "interval": "6hr", "forecast_horizon": "7day",
             "start_datetime": "2024-10-01T00:00:00Z",      // cold-start anchor when there is no snapshot
             "catch_up": true, "data_latency": "1hr", "check_interval": "15min",
             "pre_cycle_command": "sh ../../tools/feeds.sh", "pre_cycle_timeout_s": 1800 }
```

With `catch_up` each cycle advances to the latest interval boundary at least `data_latency` behind the wall clock,
in one step however far that is, and is skipped when no new boundary is reachable; the timer looks every
`check_interval`. A cold start therefore spins the model up from `start_datetime` to the present in its first
cycle, and a twin that was stopped catches up in one cycle when restarted. `pre_cycle_command` runs through the
shell in the deployment folder before each cycle (typically the rain, ET and observation feeds); a failure is
logged and the cycle runs on the data at hand.

### Viewer files

```json
"viewer": { "config": "../../viewer_config.json", "web_dir": "web", "observations_dir": "observations" }
```

`DTViewerWriter` writes, every cycle, the files of the OpenWatershedTwin web viewer (`status.json`,
`map_state.json`, `units/<id>.json`, `gages/<id>.json`, plus the config and GIS layers) into `web_dir`. The
viewer config says which kernel output feeds each variable; the kernel must be generated with those outputs
(`ohq_generate --outputs`). Values of each Advance stage are kept in a rolling history
(`state/viewer_history.bin`, `output_times.history_days`); the files combine it with the Forecast stage and are
replaced atomically.
