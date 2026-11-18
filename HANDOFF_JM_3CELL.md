# Session handoff — JM corrected 3-cell digital twin + live sensor assimilation (2026-11-11, updated 2026-11-13)

## Where things stand in one paragraph

The site has only 3 constructed cells, not the 4 the original model assumed
(confirmed on site). Two pages are live on `openhydrotwin.com`, in full
isolation from the live Bioretention and original JM services: a standalone
forecast (uncalibrated baseline) and an assimilation page fed by a new
bridge that feeds it real field sensor data instead of a synthetic truth
run. A third page, `JM_truth_3cell`, was built and deployed initially but
has since been **retired from the server** (2026-11-13) — see "Why only two
pages now" below. Its deployment definition is kept in the repo as the
synthetic-reference option, just not currently running.

## URLs (live)

| Page | URL |
|---|---|
| Forecast (7-day, no sensor correction) | `https://openhydrotwin.com/JM_Bioretention_forecast_3cell/` |
| Assimilation (real sensor-corrected) | `https://openhydrotwin.com/JM_Bioretention_assimilation_3cell/` |
| Sensor bridge (proxy, not a viewer page) | `https://openhydrotwin.com/JM_sensor_bridge_3cell/outputs/selected_output.csv` |

`JM_truth_3cell` (`.../JM_Bioretention_truth_3cell/`) now 404s by design —
retired, not broken.

None of these touch the live `/JM_Bioretention_forecast/` page or the
Bioretention services — see Isolation below. `--set-landing` was never
passed, so the site's root/landing target is unchanged.

## Why only two pages now

The general OHTwin pattern (confirmed against Bioretention's own live
deployment list: only `Bioretention_truth` + `Bioretention_assimilation`,
no separate forecast process at all) is two coupled processes, not three —
"forecast" is just the forward-looking stage every deployment already
produces each cycle (`forward_advance` + `forward_forecast`), so
assimilation's own forecast output naturally reflects whatever calibration
just happened, with no separate wiring needed.

JM deviated from that by running three fully independent processes. Once
`JM_truth_3cell`'s noise was zeroed (per Arash's request — see "Noise" below),
it became byte-for-byte functionally identical to `JM_forecast_3cell`:
same model file, same config apart from name/port, same zero noise, no
coupling to anything either way. Running two processes that produce the
exact same output forever was pure duplication, so truth was retired from
the server on 2026-11-13 (`systemctl stop` + `disable`, nginx location file
removed, published viewer page directory removed, all validated and
reloaded cleanly). `deploy_jm_truth_3cell.sh` still works if anyone wants
to redeploy it later; nothing about the fork was deleted, just unpublished.

## Noise

`JM_truth_3cell`'s `config.json` has non-zero `noise_sigma` on every
observation (0.05 on Soil 1/2/3 moisture, 0.1–0.02 on others) — synthetic
multiplicative noise, not sensor data, matching Bioretention's own `truth`
convention. Arash flagged this as erratic-looking; briefly zeroed it out
and verified the clean output, then reverted and kept the original noisy
config since truth is retired from the server anyway and no longer
displayed to anyone.

## Why 3 cells, not 4

The as-designed BP-01 profile had four columns (Street Gutter / Pond /
Engineered Soil / Aggregate, numbered 1–4). Column 4 — the lowest-elevation
one — was never built in the field (confirmed on site; column 1 is the
highest elevation and is where the sensors are physically installed).
Column 4's blocks, links, and observations are removed from the model;
DA-05's runoff and Street Gutter 3's surface overflow are redirected the
same way column 4 handled them (mirrors how DA-02/DA-03 already share
Street Gutter 2); the underdrain is merged into one pipe running Aggregate 3
straight to the Catch Basin, keeping the original start/end inverts.

## Deployments and how they were built

- `deployments/JM_truth_3cell/`, `JM_forecast_3cell/`, `JM_assimilation_3cell/`
  — corrected copies of the original `JM_truth`/`JM_forecast`/`JM_assimilation`.
- `JM_assimilation_3cell/model/JM.ohq` and `JM_truth_3cell/model/JM.ohq` were
  both ultimately replaced wholesale with the corrected `JM_forecast_3cell`
  model — their own originals were incomplete drafts (zero `create
  observation` lines, no overflow weirs, no evapotranspiration wiring,
  output path pointing at an unrelated project's directory). This is a
  pre-existing gap in the source deployments they were copied from, not
  something introduced during the 3-cell correction.
- `JM_truth_3cell` was originally built on the assumption that Bioretention
  follows a three-page (truth/forecast/assimilation) convention. That
  assumption turned out to be wrong — checked Bioretention's actual deployment
  list and it only has two (`Bioretention_truth` + `Bioretention_assimilation`,
  no separate forecast process at all; see "Why only two pages now" above).
  `JM_truth_3cell`'s calibration role (feeding a synthetic "truth" to
  assimilation) was already superseded by the real sensor bridge below;
  nothing ever pointed `truth_csv_url` at it. Combined with its noise being
  zeroed (making it identical to forecast), it's now retired from the server.

## The live sensor bridge (new)

`deployments/JM_assimilation_3cell/bridge/sensor_bridge.py` — a small
stdlib-only Python HTTP server, deployed to the twin server as its own
systemd service (`jm-sensor-bridge-3cell`, bound to `127.0.0.1:8198`). It
polls the real AnacostiaIQ field API
(`http://54.213.147.59:5000`, project `John-McCormack`, the replacement
field Pi `jm-cua-twin-pi`, live since 2026-10-30) and republishes two
sensors in the exact shape the assimilation engine expects:

- **MaxBotix "Ponding Depth" → "Pond 1 water depth"** — sensors are
  physically installed in the highest-elevation cell only, which is
  Pond 1 / Engineered Soil 1 in this model.
- **Moisture probe A0 → "Soil 1 moisture"**.

Caveats carried in the bridge's own docstring and the config's `_comment`,
not just here:
- Depth datum (field's own zero reference vs. the model's elevation-based
  pond depth) is **not reconciled** — only a unit conversion is applied.
- Moisture scale (field's 0–100% dry/wet calibration vs. the model's
  volumetric theta) is a linear placeholder, not a real soil calibration.
- The pressure transducer (planned for the deep inspection well) isn't
  wired in yet — the model has no "inspection well" object distinct from
  the pond surface.
- As of 2026-11-11, the MaxBotix is still physically at the deep well
  (confirmed via the Pi's actively-running config, `totalLength` unchanged)
  — the planned move to the perforated pipe, with a pressure transducer
  taking the well, had not happened yet. **Re-check this before trusting
  the mapping if picking this back up later** — once it moves, MaxBotix
  stops measuring pond depth and the bridge's mapping needs to change.
- Only the one physically-instrumented cell is wired into
  `calibration_observations`; the other two columns keep simulating but
  carry no assimilation pressure. Estimating them from the one
  instrumented cell is future work.

## Isolation from live services

Three deploy scripts — `deploy_jm_3cell.sh`, `deploy_jm_truth_3cell.sh`,
`deploy_jm_assimilation_3cell.sh` — share one tree and systemd template,
entirely separate from both the live Bioretention services and the original
(uncorrected) `JM_forecast`:

- Tree: `/home/ubuntu/jm_twin_3cell/` (not `/home/ubuntu/jm_twin/` or
  `/home/ubuntu/drywelldt/`).
- Systemd template: `drywelldt-jm-3cell@.service` (not `drywelldt@` or
  `drywelldt-jm@`) — forecast/assimilation are two instances of it (truth
  was a third instance, now stopped and disabled — see "Why only two pages
  now" above).
- Bridge: its own standalone unit, `jm-sensor-bridge-3cell.service`, own
  tree `/home/ubuntu/jm_sensor_bridge_3cell/`.
- nginx: each gets its own `/etc/nginx/ohtwin-locations/*.conf`, named
  after the deployment, so none can collide with existing files. The
  bridge's page is the one new pattern — `proxy_pass` to its local port,
  since (unlike everything else here) it computes its response fresh per
  request rather than serving a file OHTwin already wrote to `outputs/`.
- Verified repeatedly throughout: `drywelldt@Bioretention_truth`,
  `drywelldt@Bioretention_assimilation`, and `drywelldt-jm@JM_forecast`
  stayed `active running` with zero restarts through every redeploy here.

## Bugs found and fixed

1. **Stale 4-cell diagram data.** `JM_truth_3cell/viz_jm.json` was never
   updated when the model was corrected to 3 cells — the engine ran fine
   (same corrected model either way), but the rendered diagram still showed
   4 cells. Fixed by copying the already-corrected `forecast_3cell` version.

2. **Mixed content hung every page forever.** All three deploy scripts
   generated `config.json` with `http://` URLs, but the pages are actually
   served over `https://`. An `https` page with COEP active (required for
   this Qt WASM build's `crossOriginIsolated`) silently blocks fetching
   `http://` subresources — the viewer never got past its own first data
   fetch, stuck at the loading splash indefinitely. Fixed by emitting
   `https://` consistently (matching what the already-working original
   pages' own `config.json` used — that was the tell).

3. **(Also fixed, not a visible bug but worth noting) The local dev
   machine's own prebuilt WASM viewer bundle was a different, unverified
   build** (confirmed different byte size from the proven production one),
   and scp-ing a 16MB file from this specific dev machine was unreliable —
   a transfer was observed truncating at ~300KB. All three scripts now
   source the viewer bundle server-side, from a known-good reference copy
   staged once at `/home/ubuntu/known_good_viewer` on EC2, rather than
   pushing anything large from this machine.

4. **Shared-binary redeploy race (`Text file busy`).** All three
   deployments share one binary path; redeploying one while another was
   running failed with `ETXTBSY`. Fixed with an atomic push (scp to a side
   name, then `mv` into place — a rename, not a write, safe even while the
   old inode is still mapped into the other running processes).

## Assimilation failures diagnosed 2026-11-17

The previous explanation that an empty viewer was merely an unlucky saturated
soil snapshot was incorrect. The failing snapshot's four soil moisture values
were within their residual/saturated bounds. An isolated reproduction of
`LoadfromJson` followed by `VerifyAllQuantities` yielded four errors: derived
Storage was still zero. Calling `CalcAllInitialValues` before validation yielded
zero errors, without modifying theta or relaxing any criteria. The calibration
pre-flight now initializes derived state before validating the loaded snapshot.

A separate bridge routing defect rejected the viewer's cache-busting query
strings (`selected_output.csv?_t=...`) with HTTP 404. Route matching now uses
only the parsed URL path; CSV and metadata queries both work. Regression tests
are in `tests/test_jm_sensor_bridge.py`.

Weather requests also used only `forecast_days=10`, omitting the historical
part of the 14-day sensor window. Both Open-Meteo forecast fetchers now request
explicit UTC start/end dates. A live API check returned all five required hourly
variables for 2026-11-03 through 2026-11-17 (360 samples each).

A negative regression check also confirms that a deliberately invalid soil
moisture value remains rejected after initialization; validation was not
bypassed or loosened.

Calibration was configured to retry every three days, so a running service did
not imply successful calibration. A new GA deployment now schedules an initial
calibration once its first forward snapshot arrives if no merged GA history
exists. The resume path now passes its existing snapshot to assimilation even
when forward advancement is waiting for wall-clock time or observations.
Established deployments retain the configured polling cadence. Check
actual calibration artifacts and the run log before describing this page as
sensor-corrected. Real sensor mapping
remains limited to Cell 1 (MaxBotix and A0); depth datum and volumetric moisture
conversion remain provisional as documented above.

## Loose ends / future work

- `config_viewer.json` is for a separate downstream AssimViewer dev tool,
  not the live page — inherited stale ports from the never-deployed
  original `JM_assimilation`, not yet corrected (noted but out of scope
  for the live-page work above).
- MaxBotix→pond-depth and moisture→soil-theta mappings are both honest
  placeholders (see bridge caveats above) — revisit once the MaxBotix
  physically moves to the perforated pipe and/or a real pressure
  transducer goes into the deep well.
- `deployment.port` in every deployment's `config.json` is local-dev-only
  (not read by the engine at all — see the main repo `README.md`), and is
  expected to differ per developer machine; use
  `git update-index --skip-worktree` on a file if that keeps showing up as
  a spurious local diff.

## Assimilation viewer repair — 2026-11-18

The first full calibration completed on 17 November at 23:40 UTC, but the browser
still displayed no data. Its explicit error was `ga_output_merged.txt parsed but
produced no cycles.` The GA archive changed the objective header from `likelihood`
to `neg_log_likelihood`; GaMergedLoader required only the historical spelling and
rejected every generation header. It now accepts both spellings. Native tests
cover both, and the actual server archive loads one cycle with four parameters
and the two used observation metrics. Fitness and best-parameter lines also show
point markers so the single first cycle is visible without a line segment.

The patched WASM viewer is isolated to the assimilation page under
`assets-ga-objective-v2/`. Its previous HTML/assets are backed up at
`/home/ubuntu/jm_viewer_backups/assimilation-before-ga-objective-v2`.
Transfers were SHA256-verified before atomically switching the page HTML.
The forecast page and model processes were not restarted. The assimilation
deploy script now uses `/home/ubuntu/known_good_viewer_ga_objective_v2` by default,
with an environment override, to avoid restoring the incompatible viewer on a
future deployment. Existing versioned assets are retained for open browser tabs.

This repairs display/parsing only. The first fitted metrics are poor (negative
NSE for both fitted observations), and the observation-to-model mappings remain
provisional. Showing calibration results does not establish predictive validity.

## 2026-11-18: field observation mapping and shallow soil epoch

See `deployments/JM_assimilation_3cell/OBSERVATION_MAPPING.md`. Real observations
remain distinct from model forcing. The bridge now exposes all three moisture
voltages/indices, a fixed A0/A2 shallow target, MB depth, HC head/inlet flow and
pressure current. HC is compared with the Gutter 1 → Pond 1 link only as a
diagnostic while leaves remain; pressure is not converted using the borrowed
lab calibration. No sensor values are assigned to Cells 2/3.

The assimilation soil is explicitly split into upper 0.3048 m and lower 0.6096 m
layers, preserving total volume with vertical and proportionate horizontal
exchange. New target is `Cell 1 shallow moisture index`, not an asserted VWC.
The index-to-model-saturation operator and MB zero remain provisional.

Deployment at 15:13 UTC started a fresh layered-model state after backing up old
state/snapshots/model/config/outputs under
`/home/ubuntu/jm_twin_3cell/backups/before-shallow-20261118`. Forecast was untouched.
New GA output uses `outputs/calibration_shallow_v1`; the viewer clearly shows
pending recalibration and current mapping until that archive exists. It does not
show old fitness as a result for the new observation definition. The first start
raced the bridge; after confirming bridge readiness, assimilation was restarted
successfully. Deployment now waits for bridge readiness before starting it.
Server confirmed two matched targets and 99,055 buffered points across 11 series.
New GA started 15:13:42 UTC; fit quality is not yet known. Do not report fully fixed
or better predictive performance before evaluating its completed results.

Checks: 21 JM Python tests passed; native OHQ constructed/validated/solved the
23-observation layered model; layer depth/continuity/routing checks passed.
The existing schematic has not been redrawn to show the lower soil nodes.

User requested keeping the page visible while calibration runs. The root page now shows an explicitly labeled previous-calibration checkpoint from `previous-calibration/`; new results are staged under `latest/` with the revised config. The status script navigates there once the new GA archive exists. No calibration service was restarted for this presentation change.

## 2026-11-18: show all measured diagnostic observations

The comparison viewer incorrectly cleared measured series unless they belonged
to the completed GA's calibration set. It now displays every available measured
series, includes its values in axis ranges, and explicitly labels non-fit series
as diagnostic. Calibration membership and weights were not changed. WASM startup
config fetch now uses a cache-busting timestamp to avoid stale observation URLs.

Root page keeps previous fitness/parameters, but Comparison now uses the current
bridge and provisional layered-model output. The banner distinguishes these.
The new completed-calibration page uses `reanalysis_output.csv` for comparison
with its fitted historical model, rather than the pre-calibration forecast.
Viewer assets are `assets-observations-v3/`; known-good flat bundle is
`/home/ubuntu/known_good_viewer_observations_v3`. Calibration service was not
restarted for the viewer change.

## 2026-11-18: signed groundwater exchange display

Groundwater recharge is the signed native-soil → fixed-groundwater link flow,
`(head.s-head.e)*K_eff/length*area`, in m³/day. Negative values represent upward
exchange, not invalid telemetry. Comparison incorrectly forced all Y axes to
start at zero and clipped the negative part (sampled range -0.000917505 to
+0.00191883 m³/day). Axis bounds now preserve negative data while retaining a
zero floor for nonnegative series. Label clarifies positive recharge / negative
upward exchange. This remains model-only; there is no recharge observation.

Assets: `assets-signed-flow-v4/`; flat deployment reference:
`/home/ubuntu/known_good_viewer_signed_flow_v4`. Only viewer assets changed;
no model/parameter/data changes or calibration restart.
The visible step at the November 18 model restart is a cold-start/model-epoch
transition: selected_output.csv retains older model history followed by the new
layered-model forecast. It is not a measured groundwater jump. Historical model
output remains preserved; no new recharge sensor has been introduced.

Chart visibility cache recovery: the status script URL is now versioned as
`calibration_status.js?v=visible-v5`, and it removes the superseded exact
`#screen { visibility: hidden !important; }` style if present. The chart page
rendered normally in the inspected browser; an older cached pending-calibration
script was a possible cause of the user's blank display, not conclusively proven.
Fresh entry URL: `/JM_Bioretention_assimilation_3cell/?v=visible-v5`.

Public chart simplification (2026-11-18): `?view=public` on the bridge CSV omits
A0/A1/A2 voltage and pressure loop-current diagnostics. Root and latest viewer
configs use this public feed. Moisture indices, water levels and inlet flow remain
visible. The default full bridge feed, raw cloud records and calibration input
are unchanged. Bridge service was restarted; assimilation PID stayed 1723100.
Nine bridge tests passed, including separation of public and full responses.
