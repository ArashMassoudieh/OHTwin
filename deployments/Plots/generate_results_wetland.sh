#!/usr/bin/env bash
# =============================================================================
# generate_results_wetland.sh
#
# Regenerates Wetland-derived result artifacts from Wetland_assimilation and
# Wetland_truth. Intended to be run from the deployments/ directory.
#
# Produced files use the _wetland suffix to avoid overwriting Bioretention plots.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

echo "[generate_results_wetland] working directory: $(pwd)"


# Optional bundled drift truth file for soil Ksat.
# In the deployment this normally lives under Wetland_truth/drift/.
if [[ -f "ksat_drift_wetland.csv" && ! -f "Wetland_truth/drift/ksat_drift_wetland.csv" ]]; then
    mkdir -p "Wetland_truth/drift"
    cp "ksat_drift_wetland.csv" "Wetland_truth/drift/ksat_drift_wetland.csv"
fi

echo "[generate_results_wetland] building fitness_history_wetland.csv ..."
python3 build_fitness_history_wetland.py \
    --ga  "Wetland_assimilation/outputs/calibration/ga_output_merged.txt" \
    --out "Wetland_assimilation/outputs/calibration/fitness_history_wetland.csv"


echo "[generate_results_wetland] preparing normalized wetland plot inputs ..."
python3 prepare_wetland_plot_inputs.py \
    --truth "Wetland_truth/outputs/selected_output.csv" \
    --assim "Wetland_assimilation/outputs/selected_output.csv" \
    --reanalysis "Wetland_assimilation/outputs/reanalysis_output.csv" \
    --outdir "Wetland_assimilation/outputs/paper_plot_inputs"

echo "[generate_results_wetland] rendering paper_fitness_history_wetland.png ..."
gnuplot plot_paper_fitness_history_wetland.gp

echo "[generate_results_wetland] rendering paper_physical_parameters_wetland.png ..."
gnuplot plot_paper_physical_parameters_wetland.gp

echo "[generate_results_wetland] rendering paper_sigma_estimates_wetland.png ..."
gnuplot plot_paper_sigma_estimates_wetland.gp

echo "[generate_results_wetland] rendering paper_truth_vs_assim_wetland.png ..."
gnuplot plot_paper_truth_vs_assim_wetland.gp

echo "[generate_results_wetland] rendering paper_truth_vs_assim_diagnostics_wetland.png ..."
gnuplot plot_paper_truth_vs_assim_diagnostics_wetland.gp

echo "[generate_results_wetland] rendering paper_wetland_stage_three_views.png ..."
gnuplot plot_paper_wetland_stage_three_views.gp

echo "[generate_results_wetland] done."
