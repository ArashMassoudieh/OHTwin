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

# Optional argument: --drift selects the Bioretention_*_drift case.
# Pass `drift=1` through to each gnuplot script and point build_fitness_history.py
# at the drift calibration folder. PNG names get a `_drift` suffix.
DRIFT=0
case "${1:-}" in
    "")        DRIFT=0 ;;
    --drift)   DRIFT=1 ;;
    -h|--help)
        echo "Usage: $0 [--drift]"
        echo "  (no argument) : nominal case (Bioretention_assimilation/, *.png)"
        echo "  --drift       : drift case   (Bioretention_assimilation_drift/, *_drift.png)"
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        echo "Usage: $0 [--drift]" >&2
        exit 2
        ;;
esac

if [[ "$DRIFT" -eq 1 ]]; then
    GNUPLOT_ARGS=( -e "drift=1" )
    FITNESS_ARGS=( --ga  "Bioretention_assimilation_drift/outputs/calibration/ga_output_merged.txt"
                   --out "Bioretention_assimilation_drift/outputs/calibration/fitness_history.csv" )
    CASE_LABEL="drift"
else
    GNUPLOT_ARGS=()
    FITNESS_ARGS=()
    CASE_LABEL="nominal"
fi

# Run from the directory containing this script, so relative paths in the
# gnuplot scripts and CSV defaults resolve correctly regardless of where
# the user invokes us from.
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

echo "[generate_results] working directory: $(pwd)  (case: $CASE_LABEL)"

# -----------------------------------------------------------------------------
# Step 1: build fitness_history.csv from ga_output_merged.txt
# -----------------------------------------------------------------------------
echo "[generate_results] building fitness_history.csv ..."
python3 build_fitness_history.py "${FITNESS_ARGS[@]}"

# -----------------------------------------------------------------------------
# Step 2: render paper figures
# -----------------------------------------------------------------------------
echo "[generate_results] rendering paper_fitness_history.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_fitness_history.gp

echo "[generate_results] rendering paper_physical_parameters.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_physical_parameters.gp

echo "[generate_results] rendering paper_sigma_estimates.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_sigma_estimates.gp

echo "[generate_results] rendering paper_truth_vs_assim.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_truth_vs_assim.gp

echo "[generate_results] rendering paper_truth_vs_assim_diagnostics.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_truth_vs_assim_diagnostics.gp

echo "[generate_results] rendering paper_soilmoisture_three_views.png ..."
gnuplot "${GNUPLOT_ARGS[@]}" plot_paper_soilmoisture_three_views.gp

echo "[generate_results] done."
