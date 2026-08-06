# JM truth and future-assimilation plotting package

Place these files beside `JM_truth/` and, later, `JM_assimilation/`.

## Run now

```bash
chmod +x generate_results_jm.sh
./generate_results_jm.sh
```

Default `ASSIMILATION_MODE=auto` plots truth only when no assimilation CSV exists. Later it automatically overlays `JM_assimilation/outputs/selected_output.csv`.

```bash
ASSIMILATION_MODE=off ./generate_results_jm.sh
ASSIMILATION_MODE=on ./generate_results_jm.sh
ASSIM_DIR=JM_assimilation_MCMC ASSIMILATION_MODE=auto ./generate_results_jm.sh
WINDOW_DAYS=7 ./generate_results_jm.sh
```

The parser automatically detects all `Pond N water depth`, `Soil N moisture`, `Pond N infiltration`, and `Pond N overflow` outputs for any N. It also detects precipitation, DA-01 runoff, Gutter 4 depth, underdrain outlet flow, groundwater recharge, catch basin depth, and catch basin outlet flow.

Outputs are written to `JM_plot_results/`, including grouped pond, soil, infiltration, overflow, system-flow, hydraulic-state, and early/full/late summary figures.
