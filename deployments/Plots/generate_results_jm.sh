#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

TRUTH_DIR="${TRUTH_DIR:-JM_truth}"
ASSIM_DIR="${ASSIM_DIR:-JM_assimilation}"
ASSIMILATION_MODE="${ASSIMILATION_MODE:-auto}"   # auto | on | off
WINDOW_DAYS="${WINDOW_DAYS:-10}"
OUTPUT_DIR="${OUTPUT_DIR:-JM_plot_results}"
PLOT_CONFIG="${PLOT_CONFIG:-jm_plot_config.json}"

case "$ASSIMILATION_MODE" in
  auto|on|off) ;;
  *)
    echo "[error] ASSIMILATION_MODE must be auto, on, or off" >&2
    exit 2
    ;;
esac

TRUTH_FILE="$TRUTH_DIR/outputs/selected_output.csv"
ASSIM_FILE="$ASSIM_DIR/outputs/selected_output.csv"

if [[ ! -f "$TRUTH_FILE" ]]; then
  echo "[error] truth output not found: $TRUTH_FILE" >&2
  exit 1
fi

INCLUDE_ASSIM=0
if [[ "$ASSIMILATION_MODE" == "on" ]]; then
  if [[ ! -f "$ASSIM_FILE" ]]; then
    echo "[error] assimilation mode is on, but file is missing: $ASSIM_FILE" >&2
    exit 1
  fi
  INCLUDE_ASSIM=1
elif [[ "$ASSIMILATION_MODE" == "auto" && -f "$ASSIM_FILE" ]]; then
  INCLUDE_ASSIM=1
fi

mkdir -p "$OUTPUT_DIR"

ARGS=(
  --truth "$TRUTH_FILE"
  --output-dir "$OUTPUT_DIR"
  --window-days "$WINDOW_DAYS"
  --config "$PLOT_CONFIG"
)

if [[ "$INCLUDE_ASSIM" -eq 1 ]]; then
  ARGS+=(--assimilation "$ASSIM_FILE")
  echo "[jm-plots] assimilation overlay enabled"
else
  echo "[jm-plots] truth-only mode"
fi

python3 plot_jm_results.py "${ARGS[@]}"

echo "[jm-plots] done"
echo "[jm-plots] figures: $OUTPUT_DIR"
