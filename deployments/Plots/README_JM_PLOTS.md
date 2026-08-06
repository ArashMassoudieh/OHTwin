# JM truth and assimilation plotting package

Place this package in the directory containing the deployment directories:

- `JM_truth/`
- optionally, in the future, `JM_assimilation/`

The package reads:

- `JM_truth/outputs/selected_output.csv`
- optionally `JM_assimilation/outputs/selected_output.csv`

## Install requirements

```bash
python3 -m pip install matplotlib
```

## Current truth-only run

```bash
chmod +x generate_results_jm.sh
./generate_results_jm.sh
```

Explicitly disable assimilation:

```bash
ASSIMILATION_MODE=off ./generate_results_jm.sh
```

## Future assimilation overlay

When `JM_assimilation/outputs/selected_output.csv` exists:

```bash
ASSIMILATION_MODE=on ./generate_results_jm.sh
```

Automatic behavior is the default:

```bash
ASSIMILATION_MODE=auto ./generate_results_jm.sh
```

In `auto` mode, the script overlays assimilation results only when the
assimilation CSV exists.

## Alternative deployment names

```bash
TRUTH_DIR=JM_truth \
ASSIM_DIR=JM_assimilation_MCMC \
ASSIMILATION_MODE=auto \
./generate_results_jm.sh
```

## Automatic windows

The package reads the minimum and maximum times from the truth CSV.

Default:

- early figure: first 10 days
- late figure: last 10 days

Change the window length:

```bash
WINDOW_DAYS=7 ./generate_results_jm.sh
```

## Generated output

Figures and diagnostics are written to `JM_plot_results/`:

- `paper_jm_states_full_truth.png`
- `paper_jm_flows_full_truth.png`
- `paper_jm_early_window_truth.png`
- `paper_jm_late_window_truth.png`
- optional additional-output figure
- `jm_plot_windows.json`
- `jm_variable_inventory.md`

With assimilation enabled, filenames end in
`truth_vs_assimilation.png`.

## Variable matching

`jm_plot_config.json` contains aliases for expected JM variables such as:

- precipitation
- catchment runoff
- gutter flow
- catch-basin inflow and stage
- bioretention inflow
- surface-water depth
- media water content
- underdrain flow
- infiltration
- overflow
- groundwater state

The exact `selected_output.csv` names can differ between deployments. After the
first run, inspect:

```text
JM_plot_results/jm_variable_inventory.md
```

If a requested output was not matched, add its exact header text to the
corresponding `aliases` list in `jm_plot_config.json`.
