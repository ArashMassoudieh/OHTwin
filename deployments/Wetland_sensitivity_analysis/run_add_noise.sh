#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PY_SCRIPT="${SCRIPT_DIR}/add_ou_noise_from_config.py"

PY_CLEAN="${SCRIPT_DIR}/.add_ou_noise_from_config_clean.py"
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

CONFIG="${SCRIPT_DIR}/config.json"

OUTDIR="${SCRIPT_DIR}/noisy_observations"
SEED=12345

CSV_OUTFLOW="${SCRIPT_DIR}/WetlandOutflowObs.csv"
CSV_CELL1="${SCRIPT_DIR}/WetlandCell1WaterDepthObs.csv"
CSV_CELL6="${SCRIPT_DIR}/WetlandCell6WaterDepthObs.csv"

for f in \
    "${CSV_OUTFLOW}" \
    "${CSV_CELL1}" \
    "${CSV_CELL6}"
do
    if [[ ! -f "${f}" ]]; then
        echo "[ERROR] Missing input CSV: ${f}"
        exit 1
    fi
done

if [[ ! -f "${CONFIG}" ]]; then
    echo "[ERROR] Missing config: ${CONFIG}"
    exit 1
fi

mkdir -p "${OUTDIR}"

echo "============================================================"
echo "Adding multiplicative OU noise from config"
echo "============================================================"
echo "CONFIG = ${CONFIG}"
echo "OUTDIR = ${OUTDIR}"
echo "SEED   = ${SEED}"
echo ""

python3 "${PY_CLEAN}" \
    "${CSV_OUTFLOW}" \
    "${CSV_CELL1}" \
    "${CSV_CELL6}" \
    --config "${CONFIG}" \
    --outdir "${OUTDIR}" \
    --seed "${SEED}"

echo ""
echo "[OK] Generated:"
find "${OUTDIR}" -maxdepth 1 -name '*_OU_noisy.csv' -printf '  %f\n' | sort
