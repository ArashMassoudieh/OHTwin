# Wetland MCMC paper-plot package

Place this package in the directory that contains:

- `Wetland_truth/`
- `Wetland_assimilation_MCMC/`

Then run:

```bash
./generate_results_wetland_mcmc.sh
```

Alternative directory names can be supplied without editing files:

```bash
TRUTH_DIR=Wetland_truth MCMC_DIR=Wetland_assimilation_MCMC ./generate_results_wetland_mcmc.sh
```

## Expected MCMC inputs

- `Wetland_assimilation_MCMC/outputs/selected_output.csv`
- `Wetland_assimilation_MCMC/outputs/reanalysis_output.csv`
- `Wetland_assimilation_MCMC/outputs/calibration/posterior_history.jsonl`
- `Wetland_assimilation_MCMC/outputs/calibration/parameter_ci_history.csv`
- `Wetland_truth/outputs/selected_output.csv`
- optional drifting truth: `Wetland_truth/drift/ksat_drift_wetland.csv`

## Generated intermediate files

Written to `Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/`:

- `truth_normalized_mcmc.csv`
- `mcmc_normalized.csv`
- `reanalysis_normalized_mcmc.csv`
- `mcmc_diagnostics_wetland.csv`
- `mcmc_physical_parameters_wetland.csv`
- `mcmc_sigma_parameters_wetland.csv`

## Generated figures

- `paper_mcmc_diagnostics_wetland.png`
- `paper_physical_parameters_mcmc_wetland.png`
- `paper_sigma_estimates_mcmc_wetland.png`
- `paper_truth_vs_mcmc_wetland.png`
- `paper_truth_vs_mcmc_diagnostics_wetland.png`
- `paper_wetland_hrt_full_period_mcmc.png`
- `paper_wetland_stage_three_views_mcmc.png`

The truth-vs-MCMC main figure uses the configured MCMC calibration targets (Cell 1 depth, Cell 6 depth, and outflow) plus inlet stage for hydraulic context. The separate diagnostic figures retain mid-stage, HRT, outlet stage, and reanalysis views.

Edit `x0`, `x1`, `x2`, and `x3` in the windowed `.gp` files to change the early and late paper windows.
