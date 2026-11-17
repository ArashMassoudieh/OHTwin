> Update: see RAW_DATA_AUDIT.md. Raw A1 voltage has substantial jumps; the earlier membership-switch explanation alone is incomplete.

# JM model follow-up — 17 November 2026

**Decision: retain the live models. Neither fixed observation test beats persistence.**

This is retrospective sensitivity analysis of the previously inspected dataset, not independent validation. No live timing, dashboard, sensor configuration or services changed.

| Cell 1 observation | Training RMSE | Test RMSE | Persistence RMSE | Test bins |
|---|---:|---:|---:|---:|
| Changing consensus (original) | 9.37 | 24.47 | 6.20 | 280 |
| fixed_three_mean | 4.64 | 14.91 | 2.76 | 288 |
| a1_only_control | 9.02 | 28.57 | 7.55 | 288 |

Errors are percentage points of the provisional dry/wet index. Each row has its own target; compare a model with its same-row persistence control. Cross-row errors are not a ranking of sensor accuracy.

## Findings

- 30 of 31 observation jumps larger than five points coincide with a change in the consensus probe membership. This association identifies an observation-construction artifact; it does not establish a hardware fault.
- Fixed three-probe averaging avoids changing membership but does not establish a physically correct bulk-soil observation.
- Recorded weather supplies only 5.8 mm over the full window and zero rainfall during the three-day test. This cannot establish storm-response skill.
- Both controls still show the model drying much more than observed. The assumed proportional mapping from sensor index to drainable storage needs justification; probe-specific endpoints, depth and local moisture conditions remain uncertain.
- Selecting A1 alone did not fix the mismatch. Do not declare A2 faulty or discard it on this evidence.

## Next steps

Confirm probe installation depths and dry/wet calibration; use that to define the observation-to-storage mapping. Then test state assimilation separately from parameter calibration on a prospectively declared dry period and rainfall event. See VALIDATION_PLAN.md for timing, data-leakage and promotion requirements.

Historical replay is already accelerated by running the solver without real-time waits. Full-runner acceleration is supported, but must use separate replay state and outputs. Production remains at its existing settings.

Validation: 11 focused tests passed, including fixed-membership omission rules, observation aggregation, analytic soil recession, rainfall routing and bridge query URLs. No commits or pushes were made.
