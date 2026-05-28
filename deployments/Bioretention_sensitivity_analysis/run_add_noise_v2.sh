#!/usr/bin/env bash
set -euo pipefail

# Runner for multiplicative/log-normal OU observation noise.
# Put this file beside add_ou_noise_to_obs_v2.py and the three observation CSVs,
# or edit CONFIG / CSV paths below.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PY_SCRIPT="${SCRIPT_DIR}/add_ou_noise_to_obs_v2.py"

# If your uploaded script contains non-breaking spaces, this runner creates
# a temporary cleaned copy before executing it.
PY_CLEAN="${SCRIPT_DIR}/.add_ou_noise_to_obs_v2_clean.py"
python3 - <<PY
from pathlib import Path
src = Path(r"${PY_SCRIPT}")
dst = Path(r"${PY_CLEAN}")
if not src.exists():
    raise SystemExit(f"[ERROR] Cannot find {src}")
text = src.read_text(encoding="utf-8")
text = text.replace("\u00a0", " ")
dst.write_text(text, encoding="utf-8")
PY

# Use truth config because it contains observations.noise_sigma and noise_correlation_time.
# Edit this if your config is elsewhere.
CONFIG="${SCRIPT_DIR}/config.json"
if [[ ! -f "${CONFIG}" ]]; then
    CONFIG="/mnt/3rd900/Projects/DrywellDT/deployments/Bioretention_truth/config.json"
fi

OUTDIR="${SCRIPT_DIR}/noisy_observations_v2"
SEED=12345

CSV_POND="${SCRIPT_DIR}/PondDepthObs.csv"
CSV_SOIL="${SCRIPT_DIR}/SoilMoistureObs.csv"
CSV_UNDER="${SCRIPT_DIR}/UnderdrainFlowObs.csv"

for f in "${CSV_POND}" "${CSV_SOIL}" "${CSV_UNDER}"; do
    if [[ ! -f "${f}" ]]; then
        echo "[ERROR] Missing input CSV: ${f}"
        exit 1
    fi
done

if [[ ! -f "${CONFIG}" ]]; then
    echo "[ERROR] Cannot find config JSON. Edit CONFIG in this script."
    exit 1
fi

mkdir -p "${OUTDIR}"

echo "============================================================"
echo "Adding multiplicative/log-normal OU noise"
echo "============================================================"
echo "PY_SCRIPT = ${PY_SCRIPT}"
echo "CONFIG    = ${CONFIG}"
echo "OUTDIR    = ${OUTDIR}"
echo "SEED      = ${SEED}"
echo ""

python3 "${PY_CLEAN}" \
    "${CSV_POND}" \
    "${CSV_SOIL}" \
    "${CSV_UNDER}" \
    --config "${CONFIG}" \
    --outdir "${OUTDIR}" \
    --seed "${SEED}"

echo ""
echo "[OK] Noisy observation files written to: ${OUTDIR}"
find "${OUTDIR}" -maxdepth 1 -name '*_OU_noisy.csv' -printf '  %f\n' | sort
