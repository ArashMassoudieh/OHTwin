# Raw moisture voltage audit — 17 November 2026

## Correction to the earlier diagnosis

The previous statement attributing most large jumps to the combining rule alone
was too strong. Membership changes and observation jumps are associated, but the
raw A1 voltage itself jumps. Thus switching membership is not evidence that the
underlying probes are smooth; the aggregation rule also propagates/amplifies a
real discontinuity in the measurement stream. A0 and A2 are much smoother in the
available raw record. This does not identify the physical or software cause of A1.

## Repository history and available data

AnacostiaIQ commit c56b96b (10 November 2026) added raw-passthrough diagnostic
channels. Moisture raw channels report ADS1115 volts before dry/wet conversion.
The first available raw API record for each field probe is 10 November 2026 at
21:34:47 UTC (4:34:47 p.m. Eastern). There are 9,822 records per probe through
17 November at 17:14:31 UTC in this audit. No earlier raw voltage is reconstructed
from clamped percentages.

Commit 9575c93 also records changed provisional wet endpoints for A0 and A1
(5600 and 2400 ADC counts respectively; A2 remains 6400; dry is 17155).
Commit timestamps do not prove exact deployment times. The original 14-day
experiment spans this calibration history, and must not be treated as a single
known-stable calibration epoch without checking the deployment record.

The upstream DatabaseWriter.cpp serializes values to two decimal places, so
these saved volts are rounded to 0.01 V, not full-resolution ADC counts.

| Probe | Raw voltage range | Largest adjacent 15-minute median change | Changes above 0.05 V |
|---|---:|---:|---:|
| A0 | 0.79–0.93 V | 0.01 V | 0 |
| A1 | 0.29–0.94 V | 0.50 V | 43 |
| A2 | 1.17–1.34 V | 0.01 V | 0 |

Medians require at least five valid raw readings in a UTC 15-minute bin; only
adjacent populated bins are compared. These summary thresholds are diagnostic,
not an automatic rejection rule or proof of a hardware fault.

## Implications for the model

Do not fit physical drying/wetting to the intermittent A1 jumps. First inspect
A1 raw voltage, timestamp alignment and acquisition behavior; do not smooth away
sustained shifts without understanding them. Use a stable calibration epoch and
save the untouched raw record alongside any processed observations. A0/A2 can
serve as diagnostic controls, but their different voltage levels do not establish
equal calibration, equal installation depth or equal water content.

The prior persistence comparisons remain reproducible experimental scores, but
are insufficient for promotion: they combine provisional sensor indices with an
unverified storage observation operator and a changing calibration history.
No live model, sensor configuration, services or hardware were changed.
