# AWS deployment handoff — JM (John McCormack Rd) forecast twin (2026-09-01)

Companion to `HANDOFF.md`, which covers the assimilation/MCMC science. This file
covers **what is deployed on AWS, how it gets there, and what to watch out for.**

Binary built and clean: `build-qmake/bin/OHTwin` (Qt 6.8.3).

## Where things stand in one paragraph

`deployments/JM_forecast` runs as a live forecast-only digital twin on the
openhydrotwin EC2 node, published at
`http://openhydrotwin.com/JM_Bioretention_forecast/` and linked from the public
AnacostiaIQ landing page on a **different** host. On 2026-09-01 its model was
re-synced to the corrected geometry from `deployments/JM_truth`, which fixed a
defect that had made the catch basin physically undrainable since the original
Aug 10 deploy. Deployed, verified live, and running.

## What lives where — three things, two hosts

| | Host | What it is |
|---|---|---|
| **JM forecast twin** | `52.42.223.42` (openhydrotwin.com) | The digital twin. `deploy_jm.sh` target. |
| **GreenInfraIQ** | `52.42.223.42` | Unrelated second site, own server block + webroot + Let's Encrypt cert. **Not ours to touch during a JM deploy.** |
| **AnacostiaIQ** | `54.213.147.59` | Public front door + `SensorDashboard.html` (live sensor data). Links out to the twin. |

`52.42.223.42` is an **Elastic IP** — stable across instance stop/start. The
AnacostiaIQ page hardcodes `href="http://openhydrotwin.com/JM_Bioretention_forecast/"`,
so that path is a public contract: renaming `PAGE` in `deploy_jm.sh` breaks the
"View the 7-day forecast" button with no error anywhere.

JM paths 404 on the AnacostiaIQ host. The twin was never hosted there.

## The pipeline: `./deploy_jm.sh [--fresh] [--set-landing]`

One self-contained script, 9 steps. Deliberately **separate** from `deploy.sh` so
the two live Bioretention services keep running the binary they were deployed with.

| Step | What it does |
|---|---|
| 0 | Pre-flight: key, qmake, plus a Python check refusing any config with an `assimilation` block, non-zero `noise_sigma`, or a `deployment.name` ≠ `JM_forecast` |
| 1 | Build `OHTwin` Release into `build-qmake/bin/`; warns if any source is newer than the binary |
| 2 | Viewer: reuses the prebuilt WASM by default (`SKIP_VIEWER_BUILD=1`; emsdk is not installed on this machine) |
| 3 | Stage libs, wrapper, `drywelldt-jm@.service`, viewer `config.json` into `/tmp/drywelldt_jm_bundle` |
| 4 | On EC2: make the `jm_twin` tree, stop **only** `drywelldt-jm@`, clean stale `bin_jm/` paths |
| 5 | Push binary, libs, TLS plugin, **OHQ templates**; install unit; `daemon-reload` |
| 6 | `rsync --delete` the deployment, excluding `outputs/ state/ snapshots/` |
| 7 | Publish the viewer to `/var/www/drywelldt/JM_Bioretention_forecast/`, injecting the splash into the *staged* copy |
| 8 | nginx: write `ohtwin-locations/JM_forecast.conf`, add the `include` once with a timestamped backup, `nginx -t`, roll back on failure, reload |
| 9 | `systemctl enable/restart drywelldt-jm@JM_forecast`, then print `drywelldt@*` to prove Bioretention survived |

Flags: `--fresh` wipes `outputs/ state/ snapshots/` on EC2; `--set-landing`
repoints the site root from `/Bioretention_assimilation/` to this page.

## ⚠ The nesting depth is load-bearing

`DTRunner.cpp:897` resolves OHQ templates as
`applicationDirPath() + "/../../resources/"`. The grandparent of the binary's
directory therefore decides which template set gets loaded:

```
/home/ubuntu/drywelldt/bin/OHTwin     -> /home/ubuntu/resources         (Bioretention)
/home/ubuntu/jm_twin/app/bin/OHTwin   -> /home/ubuntu/jm_twin/resources (JM)
```

Putting the JM binary one level deeper is the *entire mechanism* giving JM its own
templates. The earlier `bin_jm/` layout resolved to the shared
`/home/ubuntu/resources`, whose `Sewer_system.json` predated the Street Gutter
Segment type — so the four gutters **silently failed to instantiate on the server
while working locally.** Any change to `EC2_BIN` depth must keep this invariant.

## 2026-09-01 change: truth geometry ported into the forecast model

`deployments/JM_truth/model/JM.ohq` was updated with surveyed elevations. It was
**not** copied over the forecast model — the truth build is wired for offline
running and would have broken the live deployment three ways:

| Truth has | Why it must not reach the forecast |
|---|---|
| `timeseries=Rain_JM.txt` | Forecast needs `timeseries=` empty so the runner injects live Open-Meteo precipitation |
| `Evapotranspiration=` blank on all 4 soils, no Penman source | Forecast drives ET from live weather |
| **no `observation` objects at all** | The 27 observations produce `selected_output.csv` and drive the viewer |
| `outputfile=/mnt/3rd900/...` | Path on a different machine |

Instead, **49 fields across 30 objects** were ported onto the forecast file:
gutter inverts, pond/soil/aggregate elevations and depths, catch basin floor,
receiving-water head, all five sewer-pipe start/end elevations, and
`JM_EngineeredSoilAlpha` 1 → 3. After the port, a normalized field-by-field diff
against truth showed **only** the four intended forecast-only differences above.

### Overflow weirs — the one reconstructed piece

Truth has **no** overflow weirs; the forecast has four, and its `Pond N overflow`
observations reference them by name. Dropping them would have left four
observations pointing at nonexistent objects — the same silent-instantiation
failure as the gutter incident. Keeping the old crests was worse: they sat
*below* the corrected pond bottoms, so ponds 1–3 would have drained continuously.

Crests were re-derived from truth's own geometry using the rule in the model
header (`curb-cut crest = gutter invert + 4 in = 0.1016 m`). Two independent
checks agree:

| col | gutter invert | pond bottom | new crest | crest − pond | crest − gutter |
|---|---|---|---|---|---|
| 1 | 0.678434 | 0.569976 | **0.780034** | 0.210058 | 0.101600 |
| 2 | 0.485902 | 0.423672 | **0.587502** | 0.163830 | 0.101600 |
| 3 | 0.253492 | 0.231648 | **0.355092** | 0.123444 | 0.101600 |
| 4 | 0.062738 | 0.000000 | **0.164338** | 0.164338 | 0.101600 |

Each crest is exactly 4 in above its new gutter invert **and** preserves its
original labeled ponding depth to six decimals. The old file did *not* satisfy the
4-inch rule; truth's geometry does. **These four numbers are derived, not copied —
they are the part to re-check if the generator is ever run for a `TruthDT` build.**

### The defect this fixed

| series | before | after |
|---|---|---|
| Catch basin outlet flow | 0.0000 – 0.0000 | 0.0000 – 74.3206 |
| Underdrain outlet flow | −0.4283 – 0.2279 | 0.0000 – 71.1353 |

The old outlet invert was at `0.05 m` while the basin floor was at `−1.98 m`, so
the basin could never drain and the underdrain ran backwards. Now: floor
`−1.648384`, outlet invert `−1.598384`, receiving water `−1.748384` — it drains.

**Everything the site published between Aug 10 and Sep 1 was produced under that
broken drainage.** Deployed with `--fresh` for exactly this reason: the old
accumulated state used a different elevation datum.

## 2026-09-01 change: viz diagram (`viz_jm.json`, all three JM deployments)

The three `viz_jm.json` files are byte-identical and were patched together.

**Bug found: `evapN` shadowed `soilN`.** `VizRenderer.cpp:1039-1042` resolves a
connector endpoint by looping every component and keeping the **last** whose
`block` matches. `evap1..4` carried `block: "JM Engineered Soil N"` purely
decoratively (they have no `bind`), so they won and **eight** connectors —
every `Pond N -> Engineered Soil N` and `Engineered Soil N -> Aggregate N` —
were drawn from/to the little ET label box instead of the soil block, producing
long lines down through the ponds and soils. Fixed by dropping `block` from the
evap components. Safe because they set explicit `x`/`y`, so `layoutOverride` is
true (`VizRenderer.cpp:876`) and the missing-block path at `:942` is skipped.

**Colour scheme.** Ponds were `#A78A5E` brown, which reads as soil; soils were a
single rect filled with `water_color`, which is *always* `#3B82F6` because
`resolveColor` (`:206`) returns a hardcoded blue when a component declares no
`thresholds` — and nothing in the spec did. So every soil looked permanently
saturated and moisture was legible only from the `θ = …` text.

- Ponds: basin `#CFFAFE` light cyan, stroke `#0E7490`; water on a 4-band cyan
  ramp (`#A5F3FC` → `#0891B2`) driven by `Storage`, `fill_max` 3.5.
- Soils + native: 5-band brown ramp (`#E8DCC0` pale dry → `#7A5C30` dark wet)
  driven by `theta`, `fill_max` 0.4.

Threshold schema (`:885`): `[{"below": v, "color": c}, …]`, evaluated in order,
first match wins, falling through to the last entry's colour. `above` also works.

**Overflow weirs rerouted.** `attach: "auto"` sent them diagonally through
`soil3`/`soil4`, which is what made ponds look wired to the catch basin.
`attach: "bottom-top"` keeps them in the corridor between the pond and soil rows.
Connector/block crossings went **22 → 6**.

Remaining 6 crossings, all cosmetic: `Gutter N -> Pond N` clips its own `evapN`
label (the label sits on the curb-cut line), and `Gutter 4 -> Catch Basin` clips
`pond4` and `evap4`. Fixing the first needs the ET labels narrowed to ≤85 px and
shifted off the column centreline; the second cannot be fixed by attach mode
alone.

## ⚠ A viz-only change does not appear until the next daily cycle

The runner steps once a day. Pushing a new `viz_jm.json` and restarting the
service does **not** redraw anything — `outputs/viz.svg` keeps its old timestamp
until the next `forward_advance`. Force it:

```bash
LD_LIBRARY_PATH=/home/ubuntu/jm_twin/app/lib \
QT_PLUGIN_PATH=/home/ubuntu/jm_twin/app/plugins \
/home/ubuntu/jm_twin/app/bin/OHTwin \
  --deployment /home/ubuntu/jm_twin/deployments/JM_forecast --render-only
```

`--render-only` reads the existing `viz_state.json` and rewrites only `viz.svg`
and `forecast_viz.svg`. It does not simulate and does not touch `state/`.

## Verification performed

- Local smoke run before deploying: model instantiates, all 27 observations
  resolve (including the 4 weir-dependent overflow columns), outputs complete.
- Baseline run with the pre-change model for comparison: `act_X`/`act_Y`
  "no property" (32), "Failed to parse configuration" (2) and "0 was not found"
  (2) are **identical in both** — pre-existing engine noise, not regressions.
  Solver retries rose 4 → 7 per cycle (larger real head gradients); both converge.
- Post-deploy: page and CSV both 200; viewer loads `.wasm`/`qtloader.js`/
  `config.json` and polls data with cache-busting timestamps; `run_log.csv` shows
  `forward_advance` + `forward_forecast` both `ok`; live CSV matches the local run.
- `drywelldt@Bioretention_assimilation` and `drywelldt@Bioretention_truth` both
  still `active running`.

## Gotchas

- **`deploy.sh` has a dead key path.** It still points at the old Dropbox
  "(Selective Sync Conflict)" folder. The key is at
  `/home/arash/Dropbox/AWS_/ArashLinux.pem`; `deploy_jm.sh` uses the right one.
- **`deployment.port` in `config.json` is metadata only.** `DTConfig.cpp:169`
  validates it is positive but nothing binds it. Routing is path-based on :80.
- **`deploy_jm.sh`'s header cites `DTRunner.cpp:855`; the line is now 897.** The
  rule is unchanged, but the comment's line numbers have drifted — verify by
  grepping for `../../resources` rather than trusting the citation.
- **The OHQ generator is not on this machine.** `/mnt/3rd900/Projects/DryWellScriptGenerator`
  belongs to another box, so model edits here are hand-ports. The
  `loadtemplate; filename=/mnt/3rd900/...` lines in the `.ohq` are harmless —
  the runner redirects them via the `../../resources` rule above.
- **COOP/COEP headers are ignored in production.** openhydrotwin.com is plain
  HTTP:80, so the browser drops them with a console error. Pre-existing and
  benign; AnacostiaIQ links over `http://` too.
- **`--fresh` is the right call whenever geometry changes.** Restoring state saved
  under a different datum mixes two incompatible geometries in one series.

## Next steps, in order

1. **`deployments/JM_assimilation` still carries the old (broken-drainage) model.**
   It needs the same port before it is ever run or deployed. Note it is *also*
   missing the Penman ET source and observations that a live deployment needs.
2. Consider TLS for openhydrotwin.com. GreenInfraIQ already has a Let's Encrypt
   cert on the same box, so certbot is installed and working. This is the fix if
   the viewer's COOP/COEP headers ever need to actually take effect.
3. The updated `JM.ohq` is **uncommitted** in the working tree (29 lines changed).

## Key files

| path | what |
|---|---|
| `deploy_jm.sh` | The whole pipeline. Read its header comment first — it documents the isolation design. |
| `deployments/JM_forecast/model/JM.ohq` | The deployed model. Updated 2026-09-01. |
| `deployments/JM_truth/model/JM.ohq` | Geometry source of truth. Offline-wired; never copy wholesale. |
| `deployments/JM_forecast/config.json` | Forecast-only: no `assimilation`, all `noise_sigma` zero. Step 0 enforces this. |
| `DTRunner.cpp:897` | The `../../resources` template resolution rule. Also at `DTAssimilation.cpp:673` and `:820`. |

## Operating it

```bash
./deploy_jm.sh                # redeploy, keep accumulated state
./deploy_jm.sh --fresh        # redeploy from scratch (use after geometry changes)
```

```bash
ssh -i /home/arash/Dropbox/AWS_/ArashLinux.pem ubuntu@52.42.223.42 'sudo journalctl -u drywelldt-jm@JM_forecast -f'
```
