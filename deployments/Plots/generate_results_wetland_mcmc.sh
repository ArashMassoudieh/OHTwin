#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &>/dev/null && pwd )"
cd "$SCRIPT_DIR"
TRUTH_DIR="${TRUTH_DIR:-Wetland_truth}"
MCMC_DIR="${MCMC_DIR:-Wetland_assimilation_MCMC}"
OUTDIR="$MCMC_DIR/outputs/paper_plot_inputs_mcmc"
mkdir -p "$OUTDIR"

python3 prepare_wetland_mcmc_plot_inputs.py \
  --truth "$TRUTH_DIR/outputs/selected_output.csv" \
  --mcmc "$MCMC_DIR/outputs/selected_output.csv" \
  --reanalysis "$MCMC_DIR/outputs/reanalysis_output.csv" \
  --outdir "$OUTDIR"

python3 prepare_wetland_mcmc_history.py \
  --history "$MCMC_DIR/outputs/calibration/posterior_history.jsonl" \
  --ci "$MCMC_DIR/outputs/calibration/parameter_ci_history.csv" \
  --outdir "$OUTDIR"

for gp in \
  plot_paper_mcmc_diagnostics_wetland.gp \
  plot_paper_physical_parameters_mcmc_wetland.gp \
  plot_paper_sigma_estimates_mcmc_wetland.gp \
  plot_paper_truth_vs_mcmc_wetland.gp \
  plot_paper_truth_vs_mcmc_diagnostics_wetland.gp \
  plot_paper_wetland_hrt_full_period_mcmc.gp \
  plot_paper_wetland_stage_three_views_mcmc.gp; do
  echo "[wetland-mcmc] rendering $gp"
  gnuplot "$gp"
done
echo "[wetland-mcmc] done"
