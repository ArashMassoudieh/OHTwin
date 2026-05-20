#!/usr/bin/env bash
# =============================================================================
# generate_results.sh
#
# Regenerates all derived result artifacts for the OHTwin EM&S paper from
# the raw assimilation outputs in the deployment directories. Intended to
# be run from the `deployments/` directory.
#
# Currently produces:
#   - Bioretention_assimilation/outputs/calibration/fitness_history.csv
#       (per-cycle GA fitness measures + WLS effective sample size)
#   - paper_fitness_history.png
#       (stacked NSE / MSE / per-obs NLL trajectories)
#
# Additional figures will be appended as the Case Studies section grows.
# =============================================================================

set -euo pipefail

# Run from the directory containing this script, so relative paths in the
# gnuplot scripts and CSV defaults resolve correctly regardless of where
# the user invokes us from.
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

echo "[generate_results] working directory: $(pwd)"

# -----------------------------------------------------------------------------
# Step 1: build fitness_history.csv from ga_output_merged.txt
# -----------------------------------------------------------------------------
echo "[generate_results] building fitness_history.csv ..."
python3 build_fitness_history.py

# -----------------------------------------------------------------------------
# Step 2: render paper figures
# -----------------------------------------------------------------------------
echo "[generate_results] rendering paper_fitness_history.png ..."
gnuplot plot_paper_fitness_history.gp

echo "[generate_results] rendering paper_physical_parameters.png ..."
gnuplot plot_paper_physical_parameters.gp

echo "[generate_results] rendering paper_sigma_estimates.png ..."
gnuplot plot_paper_sigma_estimates.gp

echo "[generate_results] rendering paper_truth_vs_assim.png ..."
gnuplot plot_paper_truth_vs_assim.gp

echo "[generate_results] rendering paper_truth_vs_assim_diagnostics.png ..."
gnuplot plot_paper_truth_vs_assim_diagnostics.gp

echo "[generate_results] rendering paper_soilmoisture_three_views.png ..."
gnuplot plot_paper_soilmoisture_three_views.gp

echo "[generate_results] done."
