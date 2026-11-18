# Current field observation mapping — 2026-11-18

| Field measurement | Model comparison | Active calibration? |
|---|---|---|
| MB `staging_maxbotix_depth`, inches | `Pond 1 water depth`, meters above the pond's own zero | Yes, provisional field zero |
| A0/A2 raw voltages | `Cell 1 shallow moisture index`, upper 0.3048 m soil layer, relative index | Yes, provisional observation operator |
| A0, A1, A2 separately | Per-probe voltage and relative-index diagnostics for Cell 1 | No independent duplicate weighting |
| HC restored inlet flow, L/s | `Cell 1 inlet flow diagnostic`, Gutter 1 → Pond 1 curb-cut flow, m³/day | No: leaves in the box and restored mounting calibration remain uncertain |
| HC head above weir, inches | Weir-head diagnostic, meters; not gutter or pond depth | No matching box-storage object |
| Pressure transmitter current, mA | Pressure-current diagnostic | No field current-to-depth calibration or confirmed observation location |

These are observations, not additional loads. API weather drives modeled rainfall,
evapotranspiration and runoff. HC observation flow is multiplied by 86.4 to convert
L/s to the solver's m³/day; it is not injected into modeled runoff.

## Shallow moisture

User guidance is less than one foot burial depth in Cell 1. Exact installed depths
and soil-specific VWC calibration are not documented. The upper-foot model layer is
an explicit averaging/discretization assumption, not a measured probe depth.
The previous 0.9144 m soil layer is split into an upper 0.3048 m and lower 0.6096 m
layer in all three cells. Total area, soil depth and volume are preserved. Vertical
soil exchange connects them; horizontal exchange areas are split in proportion to
depth. Lower layers drain to the existing aggregates. All layers share the existing
soil parameters. Only the top layer has soil evapotranspiration.

Model observation is `100*(theta-theta_res)/(theta_sat-theta_res)`. This is a
provisional effective-saturation proxy for the probes' relative dry/wet index,
not proof that their percentages equal either VWC or physical saturation.

Raw voltage is converted to ADC counts with one count = 0.000125 V. Fixed endpoints:
A0 17155/5600, A1 17155/2400, A2 17155/6400 (dry/wet). Fixed calibration avoids
mixing different historical percentage definitions; it does not establish accuracy.

Each completed UTC 15-minute bin requires five valid samples per included channel.
Use its median voltage; output becomes available at the bin end. One Cell 1 target
is the equal-weight mean of A0 and A2, requiring both. Missing either leaves a gap;
there is no fallback to a single channel. A1 remains recorded and exposed separately,
but its unresolved raw-voltage jumps exclude it from this initial fit. Do not map
A1/A2 to other cells or treat three nearby probes as three independent soil states.

## MB reference

Field reference 2170 mm / 85.4330708661 in was explicitly adopted as zero on
2026-11-04. The bridge converts relative water depth to meters; it does not add the
model pond-bottom elevation. This is the correct relative-depth form, conditional
on the field reference representing the pond's zero and the target tracking its
water surface. Neither condition can be independently confirmed from telemetry.
No surveyed datum correction was invented.

## Deployment epoch and checks

The previous full-depth observation mapping/state/calibration was backed up at
`/home/ubuntu/jm_twin_3cell/backups/before-shallow-20261118` before a fresh layered
assimilation state was started. The forecast deployment was not changed.
New calibration output is `outputs/calibration_shallow_v1`, separate from the old
fit. The main viewer shows an explicitly labeled previous-calibration checkpoint while the new epoch runs. Its static previous model output and corresponding old-mapping observation data are kept separate from the new target. When the new GA archive exists, the page navigates to `latest/OHTwinViewer.html`, which uses the new calibration config.

Local native OHQ check constructed, validated and solved the layered model with 23
observations. Bridge HTTP/numeric/timestamp tests passed. Server logs confirmed two
matched calibration observations, 21 non-calibration observations skipped, and a
new GA started at 2026-11-18 15:13:42 UTC (10:13:42 Eastern). The bridge exposes 11
real-data series. A successful mapping/start is not evidence of improved fit; the
new calibration must finish and be evaluated before claiming model accuracy.

The existing schematic has not been redrawn into a six-layer diagram; consult the
model script for the explicit upper/lower soil connections.
