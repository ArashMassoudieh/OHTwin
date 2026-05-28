#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PY_SCRIPT="${SCRIPT_DIR}/add_ou_noise_to_obs.py"

CONFIG="${SCRIPT_DIR}/config.json"

OUTDIR="${SCRIPT_DIR}/noisy_observations"

SEED=12345

mkdir -p "${OUTDIR}"

python3 "${PY_SCRIPT}" \
    "${SCRIPT_DIR}/PondDepthObs.csv" \
    "${SCRIPT_DIR}/SoilMoistureObs.csv" \
    "${SCRIPT_DIR}/UnderdrainFlowObs.csv" \
    --config "${CONFIG}" \
    --outdir "${OUTDIR}" \
    --seed "${SEED}"

echo ""
echo "[OK] Noisy observation files written to:"
echo "     ${OUTDIR}"
