# Wetland forward model — interpreter vs generated C++

- interpreter (OpenHydroQual-Console, writes output.txt): **30.95 s** wall
- generated solver (WetlandForward_solver, writes CSV):   **0.991 s** wall
- speedup: **31.2×**
- window: t = 18264.00 → 18995.00 days (731 d); interpreter rows 73105, codegen rows 17546

| quantity | interp final | codegen final | max abs err | max rel err (scaled) | rel RMSE |
|---|---:|---:|---:|---:|---:|
| Rain Gauge:Storage | 3.21149 | 3.2126 | 7.739e-03 | 2.410e-03 | 2.499e-04 |
| Contributing Catchment:Storage | 123.03 | 123.851 | 5.328e+01 | 1.471e-01 | 6.839e-03 |
| Wetland Cell 1:Storage | 530.595 | 539.65 | 7.273e+01 | 6.431e-02 | 6.768e-03 |
| Wetland Cell 2:Storage | 368.303 | 374.375 | 4.872e+01 | 6.343e-02 | 6.696e-03 |
| Wetland Cell 3:Storage | 310.729 | 315.717 | 3.999e+01 | 6.274e-02 | 6.638e-03 |
| Wetland Cell 4:Storage | 149.046 | 151.396 | 1.883e+01 | 6.227e-02 | 6.599e-03 |
| Wetland Cell 5:Storage | 219.397 | 222.79 | 2.716e+01 | 6.175e-02 | 6.555e-03 |
| Wetland Cell 6:Storage | 31.8424 | 32.3271 | 3.879e+00 | 6.131e-02 | 6.516e-03 |
| Receiving Water:Storage | 221359 | 222182 | 9.605e+02 | 4.339e-03 | 2.366e-03 |
| fixed_head (1):Storage | 120384 | 120391 | 1.293e+01 | 1.074e-04 | 4.521e-05 |
| Rain Gauge:AgeTracker_1:mass | 1220.98 | 1221.88 | 1.256e+00 | 1.028e-03 | 2.995e-04 |
| Contributing Catchment:AgeTracker_1:mass | 79.3573 | 68.7949 | 1.741e+02 | 9.775e-02 | 1.697e-02 |
| Wetland Cell 1:AgeTracker_1:mass | 830.685 | 842.001 | 3.774e+02 | 6.817e-02 | 1.158e-02 |
| Wetland Cell 2:AgeTracker_1:mass | 979.077 | 1012.44 | 2.922e+02 | 5.049e-02 | 8.026e-03 |
| Wetland Cell 3:AgeTracker_1:mass | 1320.63 | 1362.72 | 4.139e+02 | 5.680e-02 | 6.327e-03 |
| Wetland Cell 4:AgeTracker_1:mass | 761.409 | 783.998 | 3.338e+02 | 7.479e-02 | 6.099e-03 |
| Wetland Cell 5:AgeTracker_1:mass | 1564.53 | 1596.47 | 8.015e+02 | 9.334e-02 | 5.946e-03 |
| Wetland Cell 6:AgeTracker_1:mass | 235.727 | 240.269 | 1.253e+02 | 9.447e-02 | 6.102e-03 |
| Receiving Water:AgeTracker_1:mass | 1.20359e+08 | 1.20745e+08 | 4.107e+05 | 3.412e-03 | 1.318e-03 |
| fixed_head (1):AgeTracker_1:mass | 8.0735e+07 | 8.07689e+07 | 4.716e+04 | 5.841e-04 | 2.739e-04 |

Worst scaled error: `Contributing Catchment:Storage` max rel 1.471e-01 (scale 362.3).
