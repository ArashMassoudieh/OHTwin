#!/bin/bash
# =============================================================================
# DrywellDT Deployment Script
# Builds runner + viewer locally, transfers to EC2
# Bundles all required shared libs for self-contained deployment
# =============================================================================

set -e

# --- Configuration ---
EC2_USER="ubuntu"
EC2_HOST="ec2-34-221-236-134.us-west-2.compute.amazonaws.com"
PEM_FILE="/home/arash/Dropbox/AWS (Selective Sync Conflict)/ArashLinux.pem"
SSH_CMD="ssh -i \"${PEM_FILE}\" ${EC2_USER}@${EC2_HOST}"
SCP_CMD="scp -i \"${PEM_FILE}\""

LOCAL_PROJECT="/home/arash/Projects/OpenHydroTwin"
LOCAL_VIEWER="${LOCAL_PROJECT}/viewer"
QT_LIB_DIR="/home/arash/Qt/6.8.2/gcc_64/lib"
OHQ_LIB_DIR="${LOCAL_PROJECT}/libs/release"

EC2_BIN="/home/ubuntu/drywelldt/bin"
EC2_LIB="/home/ubuntu/drywelldt/lib"
EC2_WWW="/var/www/drywelldt"

RUNNER_BUILD="${LOCAL_PROJECT}/build/Desktop_Qt_6_8_2-Release"
VIEWER_BUILD="${LOCAL_VIEWER}/build/WebAssembly_Qt_6_8_2_single_threaded-Release"

# --- Colors ---
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
log()  { echo -e "${GREEN}[deploy]${NC} $1"; }
warn() { echo -e "${YELLOW}[deploy]${NC} $1"; }
err()  { echo -e "${RED}[deploy]${NC} $1"; exit 1; }

# =============================================================================
# Step 1 — Build runner (Release)
# =============================================================================
log "Building DrywellDT runner (Release)..."
mkdir -p "${RUNNER_BUILD}"
cd "${RUNNER_BUILD}"
/home/arash/Qt/6.8.2/gcc_64/bin/qmake \
    "${LOCAL_PROJECT}/DrywellDT.pro" \
    CONFIG+=release \
    CONFIG-=debug
make -j$(nproc)
log "Runner built: ${RUNNER_BUILD}/DryWellDT"

# =============================================================================
# Step 2 — Build viewer (WebAssembly Release)
# =============================================================================
log "Building viewer (WebAssembly Release)..."
mkdir -p "${VIEWER_BUILD}"
cd "${VIEWER_BUILD}"
/home/arash/Qt/6.8.2/wasm_singlethread/bin/qmake \
    "${LOCAL_VIEWER}/DrywellDTViewer.pro" \
    CONFIG+=release \
    CONFIG-=debug
make -j$(nproc)
log "Viewer built."

# =============================================================================
# Step 3 — Write AWS config files
# =============================================================================
log "Writing AWS config files..."

# FIX 1: Removed duplicate opening brace '{{' — was invalid JSON
cat > "${VIEWER_BUILD}/config.json" << 'EOF'
{
    "csv_url":                "http://34.221.236.134:8081/outputs/selected_output.csv",
    "viz_url":                "http://34.221.236.134:8081/outputs/viz.svg",
    "viz_state_url":          "http://34.221.236.134:8081/outputs/viz_state.json",
    "forecast_viz_url":       "http://34.221.236.134:8081/outputs/forecast_viz.svg",
    "forecast_viz_state_url": "http://34.221.236.134:8081/outputs/forecast_viz_state.json"
}
EOF

# FIX 2: Added forecast_horizon so the runner actually generates forecast_viz.svg
cat > /tmp/drywelldt_runner_config.json << 'EOF'
{
    "script_file":        "/home/ubuntu/drywelldt/models/simple_pond_catchment.ohq",
    "load_model_json":    "",
    "state_dir":          "/home/ubuntu/drywelldt/state",
    "output_dir":         "/home/ubuntu/drywelldt/outputs",
    "model_snapshot_dir": "/home/ubuntu/drywelldt/snapshots",
    "weather_file":       "",
    "weather_source":     "openmeteo",
    "latitude":           38.91088,
    "longitude":          -76.98569,
    "noaa_office":        "LWX",
    "noaa_grid_x":        96,
    "noaa_grid_y":        70,
    "interval":           "1day",
    "forecast_horizon":   "6day",
    "start_datetime":     "",
    "state_variables": [
        {
            "variable":    "Pond:storage",
            "output_path": "/home/ubuntu/drywelldt/outputs/drywell_storage.json"
        }
    ]
}
EOF

# =============================================================================
# Step 4 — Collect Qt + OHQ shared libs to bundle
# =============================================================================
log "Collecting shared libraries..."
BUNDLE_DIR="/tmp/drywelldt_bundle"
rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}/lib"

# Qt libs needed by the runner
for lib in \
    libQt6Network.so.6 \
    libQt6Core.so.6 \
    libicui18n.so.73 \
    libicuuc.so.73 \
    libicudata.so.73; do
    if [ -f "${QT_LIB_DIR}/${lib}" ]; then
        cp "${QT_LIB_DIR}/${lib}" "${BUNDLE_DIR}/lib/"
        log "  bundled: ${lib}"
    else
        warn "  not found: ${QT_LIB_DIR}/${lib}"
    fi
done

# OHQ shared lib
if [ -d "${OHQ_LIB_DIR}" ]; then
    cp "${OHQ_LIB_DIR}"/*.so* "${BUNDLE_DIR}/lib/" 2>/dev/null || true
    log "  bundled OHQ libs from ${OHQ_LIB_DIR}"
fi

# Qt TLS plugin
mkdir -p "${BUNDLE_DIR}/plugins/tls"
cp "/home/arash/Qt/6.8.2/gcc_64/plugins/tls/libqopensslbackend.so" \
   "${BUNDLE_DIR}/plugins/tls/"
log "  bundled: libqopensslbackend.so"

# =============================================================================
# Step 5 — Write wrapper script that sets LD_LIBRARY_PATH
# =============================================================================
log "Writing runner wrapper script..."
cat > "${BUNDLE_DIR}/run_drywelldt.sh" << 'WRAPPER'
#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${DIR}/../lib:${LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${DIR}/../plugins"
# Binary looks for ../../resources relative to itself — create symlink if needed
RESOURCES_TARGET="$(dirname $(dirname ${DIR}))/resources"
if [ ! -d "${RESOURCES_TARGET}" ]; then
    mkdir -p "$(dirname ${RESOURCES_TARGET})"
    ln -sf "${DIR}/resources" "${RESOURCES_TARGET}" 2>/dev/null || true
fi
exec "${DIR}/DryWellDT" "$@"
WRAPPER
chmod +x "${BUNDLE_DIR}/run_drywelldt.sh"

# =============================================================================
# Step 6 — Create remote directory structure and clear stale state/outputs
# =============================================================================
log "Creating remote directories, clearing stale state, and stopping service..."
eval "${SSH_CMD}" << 'ENDSSH'
sudo systemctl stop drywelldt 2>/dev/null || true
mkdir -p /home/ubuntu/drywelldt/bin
mkdir -p /home/ubuntu/drywelldt/lib
mkdir -p /home/ubuntu/drywelldt/outputs
mkdir -p /home/ubuntu/drywelldt/state
mkdir -p /home/ubuntu/drywelldt/snapshots
mkdir -p /home/ubuntu/drywelldt/models
sudo mkdir -p /var/www/drywelldt
sudo chown ubuntu:ubuntu /var/www/drywelldt

# FIX 3: Clear stale outputs and state so the runner starts clean
rm -f /home/ubuntu/drywelldt/outputs/*
rm -f /home/ubuntu/drywelldt/state/*

# Remove old binary in case it's still locked from a hung process
rm -f /home/ubuntu/drywelldt/bin/DryWellDT
ENDSSH

# =============================================================================
# Step 7 — Transfer runner binary + support files
# =============================================================================
log "Transferring runner binary..."
eval "${SCP_CMD} \
    \"${RUNNER_BUILD}/DryWellDT\" \
    ${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"

log "Transferring wrapper script..."
eval "${SCP_CMD} \
    \"${BUNDLE_DIR}/run_drywelldt.sh\" \
    ${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"

log "Transferring shared libs..."
eval "${SCP_CMD} \
    \"${BUNDLE_DIR}/lib\"/* \
    ${EC2_USER}@${EC2_HOST}:${EC2_LIB}/"

log "Transferring Qt plugins..."
eval "${SSH_CMD}" "mkdir -p /home/ubuntu/drywelldt/plugins/tls"
eval "${SCP_CMD} \
    \"${BUNDLE_DIR}/plugins/tls/libqopensslbackend.so\" \
    ${EC2_USER}@${EC2_HOST}:/home/ubuntu/drywelldt/plugins/tls/"

log "Transferring resources (JSON only)..."
eval "${SSH_CMD}" "mkdir -p ${EC2_BIN}/resources"
eval "${SCP_CMD} \
    \"${LOCAL_PROJECT}/resources\"/*.json \
    \"${LOCAL_PROJECT}/resources\"/*.list \
    ${EC2_USER}@${EC2_HOST}:${EC2_BIN}/resources/"

log "Transferring viz.json..."
eval "${SCP_CMD} \
    \"${LOCAL_PROJECT}/viz.json\" \
    ${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"

log "Transferring runner config.json..."
eval "${SCP_CMD} \
    /tmp/drywelldt_runner_config.json \
    ${EC2_USER}@${EC2_HOST}:${EC2_BIN}/config.json"

# FIX 4: Patch hardcoded local paths in .ohq model file before transferring
log "Patching model script paths for EC2..."
sed \
    -e "s|/home/arash/Projects/OpenHydroQual/build/Desktop_Qt_6_8_2-Release/../../resources|/home/ubuntu/drywelldt/bin/resources|g" \
    -e "s|timeseries=/home/arash/Projects/OpenHydroTwin/outputs/[^,]*|timeseries=|g" \
    "${LOCAL_PROJECT}/models/simple_pond_catchment.ohq" \
    > /tmp/simple_pond_catchment_ec2.ohq

log "Transferring patched model script..."
eval "${SCP_CMD} \
    /tmp/simple_pond_catchment_ec2.ohq \
    ${EC2_USER}@${EC2_HOST}:/home/ubuntu/drywelldt/models/simple_pond_catchment.ohq"

# =============================================================================
# Step 8 — Transfer viewer files
# =============================================================================
log "Transferring WebAssembly viewer..."
eval "${SCP_CMD} \
    \"${VIEWER_BUILD}/DrywellDTViewer.html\" \
    \"${VIEWER_BUILD}/DrywellDTViewer.js\" \
    \"${VIEWER_BUILD}/DrywellDTViewer.wasm\" \
    \"${VIEWER_BUILD}/qtloader.js\" \
    \"${VIEWER_BUILD}/qtlogo.svg\" \
    \"${VIEWER_BUILD}/config.json\" \
    ${EC2_USER}@${EC2_HOST}:${EC2_WWW}/"

# =============================================================================
# Step 9 — Set permissions and configure nginx to serve /outputs/
# =============================================================================
log "Setting permissions and configuring nginx..."
eval "${SSH_CMD}" << 'ENDSSH'
chmod +x /home/ubuntu/drywelldt/bin/DryWellDT
chmod +x /home/ubuntu/drywelldt/bin/run_drywelldt.sh
chmod -R o+rx /home/ubuntu/drywelldt/outputs

# FIX 5: Ensure nginx serves /outputs/ from the correct directory
# Add location block only if it doesn't already exist
if ! sudo grep -q "location /outputs/" /etc/nginx/sites-enabled/drywelldt 2>/dev/null; then
    sudo sed -i '/^}/i\    location \/outputs\/ {\n        alias \/home\/ubuntu\/drywelldt\/outputs\/;\n        add_header Cross-Origin-Opener-Policy same-origin always;\n        add_header Cross-Origin-Embedder-Policy require-corp always;\n    }' \
        /etc/nginx/sites-enabled/drywelldt
    sudo nginx -t && sudo systemctl reload nginx
    echo "[deploy] nginx /outputs/ location added and reloaded"
else
    echo "[deploy] nginx /outputs/ location already present"
fi
ENDSSH

# =============================================================================
# Step 10 — Install systemd service
# =============================================================================
log "Installing systemd service..."
eval "${SSH_CMD}" << 'ENDSSH'
sudo tee /etc/systemd/system/drywelldt.service > /dev/null << 'EOF'
[Unit]
Description=DrywellDT Digital Twin Runner
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/drywelldt/bin
ExecStart=/home/ubuntu/drywelldt/bin/run_drywelldt.sh
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=drywelldt

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable drywelldt
sudo systemctl restart drywelldt
sleep 3
sudo systemctl status drywelldt --no-pager
ENDSSH

# =============================================================================
# Done
# =============================================================================
log "=================================================="
log "Deployment complete!"
log ""
log "Viewer:  http://34.221.236.134:8081/DrywellDTViewer.html"
log "Outputs: http://34.221.236.134:8081/outputs/"
log ""
log "To watch runner logs live on EC2:"
log "  ssh -i \"${PEM_FILE}\" ubuntu@${EC2_HOST}"
log "  sudo journalctl -u drywelldt -f"
log "=================================================="
