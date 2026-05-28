#!/usr/bin/env bash
set -euo pipefail

# Sensitivity-analysis terminal runner.
# Usage:
#   ./run_sa_compare.sh
#   ./run_sa_compare.sh /path/to/SA_ROOT
#   ./run_sa_compare.sh /path/to/SA_ROOT all
#   ./run_sa_compare.sh /path/to/SA_ROOT rel_delta_auc
#
# Tornado metric options:
#   all                 deterministic-difference, max, and min tornado plots, default
#   max_abs_diff_vs_det maximum absolute difference against deterministic run
#   rel_delta_max       relative change in peak response
#   rel_delta_min       relative change in minimum response
#   rel_delta_mean      relative change in mean response
#   rel_delta_auc       relative change in time-integrated response
#   rmse_vs_det         RMSE against deterministic run
#
# Extra non-overwriting MSE outputs:
#   calculated_metrics_vs_deterministic.csv
#   calculated_metrics_selected.csv
#   tornado_calc_mse.csv
#   tornado_calc_mse_<output>.png
#   tornado_mse_logsens.csv
#   tornado_mse_logsens_<output>.png
#   tornado_noisy_mse_logsens.csv
#   tornado_noisy_mse_logsens_summary.csv
#   tornado_noisy_mse_logsens_<output>.png

ROOT="${1:-.}"
TORNADO_METRIC="${2:-all}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_SCRIPT="${SCRIPT_DIR}/sa_compare_outputs.py"

if [[ ! -f "${PY_SCRIPT}" ]]; then
    echo "[ERROR] Cannot find ${PY_SCRIPT}"
    echo "Put run_sa_compare.sh and sa_compare_outputs.py in the same folder."
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[ERROR] python3 not found."
    exit 1
fi

if ! command -v gnuplot >/dev/null 2>&1; then
    echo "[WARN] gnuplot not found. PNGs will not be generated."
    echo "       CSV, DAT, and GP files will still be written."
    echo "       Install with: sudo apt install gnuplot"
fi

echo "============================================================"
echo "OpenHydroQual sensitivity analysis"
echo "============================================================"
echo "ROOT           = ${ROOT}"
echo "TORNADO_METRIC = ${TORNADO_METRIC}"
echo "PY_SCRIPT      = ${PY_SCRIPT}"
echo ""

echo "Detected run folders with observedoutput.txt:"
find "${ROOT}" -maxdepth 2 -name observedoutput.txt -printf '  %h\n' | sort || true
echo ""

python3 "${PY_SCRIPT}" \
  --root "${ROOT}" \
  --deterministic "1_Deterministic" \
  --tornado-metrics "${TORNADO_METRIC}" \
  --outputs \
    "Pond water depth (m)" \
    "Soil Moisture" \
    "Underdrain flow (m3/day)"

echo ""
echo "============================================================"
echo "Done"
echo "============================================================"
echo "Results folder: ${ROOT}/SA_Results"
echo ""
echo "Tornado files:"
find "${ROOT}/SA_Results" -maxdepth 1 \
  \( -name 'tornado_*' -o -name 'plot_tornado_*' \) \
  -printf '  %f\n' 2>/dev/null | sort || true

echo ""
echo "Tornado PNG sizes:"
find "${ROOT}/SA_Results" -maxdepth 1 -name 'tornado_*.png' -printf '  %f  %s bytes\n' 2>/dev/null | sort || true

echo ""
echo "Calculated deterministic-reference metric files:"
find "${ROOT}/SA_Results" -maxdepth 1 \
  \( -name 'calculated_metrics_*' -o -name 'tornado_calc_mse*' -o -name 'plot_tornado_calc_mse*' \) \
  -printf '  %f\n' 2>/dev/null | sort || true

echo ""
echo "Fit-file MSE log-sensitivity files:"
find "${ROOT}/SA_Results" -maxdepth 1 \
  \( -name 'tornado_mse_logsens*' -o -name 'plot_tornado_mse_logsens*' \) \
  -printf '  %f\n' 2>/dev/null | sort || true

echo ""
echo "Noisy-observation fit-file MSE log-sensitivity files:"
find "${ROOT}/SA_Results" -maxdepth 1 \
  \( -name 'tornado_noisy_mse_logsens*' -o -name 'plot_tornado_noisy_mse_logsens*' \) \
  -printf '  %f\n' 2>/dev/null | sort || true
