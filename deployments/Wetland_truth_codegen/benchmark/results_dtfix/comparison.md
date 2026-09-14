# Wetland forward model — interpreter vs generated C++

- interpreter (OpenHydroQual-Console, writes output.txt): **44.57 s** wall
- generated solver (WetlandForward_solver, writes CSV):   **1.952 s** wall
- speedup: **22.8×**
- window: t = 18264.00 → 18995.00 days (731 d); interpreter rows 73101, codegen rows 36552

| quantity | interp final | codegen final | max abs err | max rel err (scaled) | rel RMSE |
|---|---:|---:|---:|---:|---:|
| Rain Gauge:Storage | 3.21132 | 3.21212 | 3.086e-03 | 9.609e-04 | 1.523e-04 |
| Contributing Catchment:Storage | 123.597 | 123.756 | 2.446e+01 | 6.752e-02 | 2.225e-03 |
| Wetland Cell 1:Storage | 538.366 | 539.632 | 1.837e+01 | 1.625e-02 | 8.576e-04 |
| Wetland Cell 2:Storage | 373.514 | 374.363 | 1.227e+01 | 1.598e-02 | 8.464e-04 |
| Wetland Cell 3:Storage | 315.01 | 315.707 | 1.005e+01 | 1.577e-02 | 8.374e-04 |
| Wetland Cell 4:Storage | 151.063 | 151.391 | 4.723e+00 | 1.562e-02 | 8.313e-04 |
| Wetland Cell 5:Storage | 222.309 | 222.783 | 6.796e+00 | 1.545e-02 | 8.243e-04 |
| Wetland Cell 6:Storage | 32.2584 | 32.3261 | 9.684e-01 | 1.531e-02 | 8.184e-04 |
| Receiving Water:Storage | 222004 | 222117 | 1.410e+02 | 6.351e-04 | 2.966e-04 |
| fixed_head (1):Storage | 120375 | 120383 | 8.787e+00 | 7.300e-05 | 3.434e-05 |
| Rain Gauge:AgeTracker_1:mass | 1221.07 | 1221.62 | 5.886e-01 | 4.820e-04 | 2.017e-04 |
| Contributing Catchment:AgeTracker_1:mass | 68.3433 | 68.3762 | 5.107e+01 | 2.863e-02 | 2.100e-03 |
| Wetland Cell 1:AgeTracker_1:mass | 789.297 | 815.087 | 1.279e+02 | 2.329e-02 | 2.831e-03 |
| Wetland Cell 2:AgeTracker_1:mass | 953.608 | 980.68 | 9.375e+01 | 1.615e-02 | 1.526e-03 |
| Wetland Cell 3:AgeTracker_1:mass | 1298.32 | 1325.4 | 1.164e+02 | 1.577e-02 | 1.174e-03 |
| Wetland Cell 4:AgeTracker_1:mass | 748.96 | 762.786 | 1.072e+02 | 2.369e-02 | 1.161e-03 |
| Wetland Cell 5:AgeTracker_1:mass | 1537.93 | 1558.34 | 2.523e+02 | 3.004e-02 | 1.132e-03 |
| Wetland Cell 6:AgeTracker_1:mass | 231.632 | 234.595 | 3.922e+01 | 2.972e-02 | 1.226e-03 |
| Receiving Water:AgeTracker_1:mass | 1.20654e+08 | 1.20719e+08 | 6.788e+04 | 5.626e-04 | 2.508e-04 |
| fixed_head (1):AgeTracker_1:mass | 8.07465e+07 | 8.07649e+07 | 1.971e+04 | 2.441e-04 | 1.199e-04 |

Worst scaled error: `Contributing Catchment:Storage` max rel 6.752e-02 (scale 362.3).
