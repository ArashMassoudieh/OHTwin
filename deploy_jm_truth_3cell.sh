#!/bin/bash
# =============================================================================
# JM (John McCormack Rd) Bioretention — 3-cell corrected truth/reference — AWS Deploy
#
# Sibling to deploy_jm_3cell.sh (forecast) and deploy_jm_assimilation_3cell.sh.
# Completes the same three-page convention Bioretention already uses
# (deploy.sh: VIEWER_PAGES=(truth assimilation forecast)) for JM's corrected
# 3-cell model too.
#
# What "truth" means here, and what it doesn't
#   In Bioretention, Bioretention_truth generates a SYNTHETIC noisy reference
#   that Bioretention_assimilation calibrates against -- a controlled way to
#   validate the assimilation algorithm since the "truth" is just the same
#   model with known noise added.
#   For JM, that calibration role is NOT what this plays: JM_assimilation_3cell
#   calibrates against REAL field sensor data (via the bridge,
#   JM_sensor_bridge_3cell), not this deployment's output -- nothing points
#   truth_csv_url at this page. It's deployed purely as the third standalone
#   viewer page, matching Bioretention's convention: an ongoing reference
#   simulation distinct from forecast's rolling prediction and assimilation's
#   sensor-corrected view. Same model as forecast_3cell, same corrected
#   3-column geometry, differs only in its injected observation noise
#   (deployments/JM_truth_3cell/config.json's noise_sigma), which is
#   intentional here, unlike forecast where non-zero noise would be a bug.
#
# Usage:
#   ./deploy_jm_truth_3cell.sh [--fresh] [--set-landing]
#   --fresh        Wipe outputs/, state/ and snapshots/ on EC2 before starting.
#   --set-landing  Point openhydrotwin.com/ at this page. NOT passed by
#                  default.
#
# Isolation -- same guarantees as its siblings. Shares the jm_twin_3cell tree
# and the drywelldt-jm-3cell@ systemd template (another instance of it,
# exactly like forecast_3cell and assimilation_3cell already are), but its
# own page path and nginx location file:
#   systemd unit  drywelldt-jm-3cell@JM_truth_3cell  (shared template, own instance)
#   nginx         /etc/nginx/ohtwin-locations/JM_truth_3cell.conf
#   page/path     /JM_Bioretention_truth_3cell/
# =============================================================================

set -e

# --- Configuration ---------------------------------------------------------
EC2_USER="ubuntu"
EC2_PUBLIC_IP="52.42.223.42"
EC2_HOST="${EC2_PUBLIC_IP}"
DOMAIN="openhydrotwin.com"

PEM_FILE="${PEM_FILE:-/home/arash/.ssh/ArashLinux.pem}"

LOCAL_PROJECT="/mnt/3rd900/Projects/OHTwin"
LOCAL_VIEWER="${LOCAL_PROJECT}/viewer"

DEPLOYMENT="JM_truth_3cell"          # directory under deployments/
PAGE="JM_Bioretention_truth_3cell"   # URL path and /var/www subdirectory
SERVICE_PREFIX="drywelldt-jm-3cell"  # shared template with forecast_3cell/assimilation_3cell

QT_VER="${QT_VER:-6.9.0}"
QT_ROOT="/home/arash/Qt/${QT_VER}"
QT_DESKTOP="${QT_ROOT}/gcc_64"
QMAKE_DESKTOP="${QT_DESKTOP}/bin/qmake"
QT_LIB_DIR="${QT_DESKTOP}/lib"
OHQ_LIB_DIR="${LOCAL_PROJECT}/libs/release"

SKIP_VIEWER_BUILD="${SKIP_VIEWER_BUILD:-1}"
VIEWER_BUILD="${LOCAL_PROJECT}/build-wasm-viewer"

PRO_FILE="${LOCAL_PROJECT}/OHTwin.pro"
RUNNER_BUILD="${LOCAL_PROJECT}/build-qmake"
RUNNER_BIN="${RUNNER_BUILD}/bin/OHTwin"

EC2_ROOT="/home/ubuntu/jm_twin_3cell"   # same tree the other two forks use
EC2_APP="${EC2_ROOT}/app"
EC2_BIN="${EC2_APP}/bin"
EC2_LIB="${EC2_APP}/lib"
EC2_PLUGINS="${EC2_APP}/plugins"
EC2_RESOURCES="${EC2_ROOT}/resources"
EC2_DEPLOY_ROOT="${EC2_ROOT}/deployments"
EC2_WWW="/var/www/drywelldt"
NGINX_LOC_DIR="/etc/nginx/ohtwin-locations"
NGINX_SITE="/etc/nginx/sites-available/openhydrotwin"

# --- Flags -----------------------------------------------------------------
FRESH=0
SET_LANDING=0
for arg in "$@"; do
    case "$arg" in
        --fresh)       FRESH=1 ;;
        --set-landing) SET_LANDING=1 ;;
        -h|--help)     sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

SSH_OPTS=(-i "${PEM_FILE}" "${EC2_USER}@${EC2_HOST}")
SCP_OPTS=(-i "${PEM_FILE}")
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()    { echo -e "${GREEN}[jm3c-truth]${NC} $1"; }
warn()   { echo -e "${YELLOW}[jm3c-truth]${NC} $1"; }
err()    { echo -e "${RED}[jm3c-truth]${NC} $1"; exit 1; }
section(){ echo -e "${GREEN}[jm3c-truth]${NC} =================================================="; \
           echo -e "${GREEN}[jm3c-truth]${NC} $1"; \
           echo -e "${GREEN}[jm3c-truth]${NC} =================================================="; }

# =============================================================================
# Step 0 — Pre-flight
# =============================================================================
section "JM 3-cell truth deploy — ${DEPLOYMENT} -> /${PAGE}/  (fresh=${FRESH})"

[[ -f "${PEM_FILE}" ]] || err "SSH key not found: ${PEM_FILE}"
[[ -d "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}" ]] || err "Local deployment not found: deployments/${DEPLOYMENT}"
[[ -f "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" ]] || err "No config.json in deployments/${DEPLOYMENT}"
[[ -x "${QMAKE_DESKTOP}" ]] || err "qmake not found at ${QMAKE_DESKTOP}"

# Unlike forecast's check, this one does NOT refuse non-zero noise_sigma --
# injected noise in truth's own output is intentional here, not a bug.
if python3 - "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" "${DEPLOYMENT}" << 'PYCHK'
import json, sys
d = json.load(open(sys.argv[1]))
expected_name = sys.argv[2]
bad = []
if "assimilation" in d:
    bad.append("has an 'assimilation' section (truth should be a plain forward run)")
if d["deployment"]["name"] != expected_name:
    bad.append("deployment.name is %r, expected %r" % (d["deployment"]["name"], expected_name))
for b in bad:
    print(b)
sys.exit(1 if bad else 0)
PYCHK
then :; else err "config.json sanity check failed (see above)"; fi
log "config.json sanity check passed"

# =============================================================================
# Step 1 — Build runner (reuses the same build as the other two forks)
# =============================================================================
section "Building OHTwin runner (Release, Qt ${QT_VER})..."
mkdir -p "${RUNNER_BUILD}"
cd "${RUNNER_BUILD}"
"${QMAKE_DESKTOP}" "${PRO_FILE}" CONFIG+=release CONFIG-=debug
make -j"$(nproc)"
[[ -x "${RUNNER_BIN}" ]] || err "Build produced no binary at ${RUNNER_BIN}"
log "Runner: ${RUNNER_BIN}"

# =============================================================================
# Step 2 — Viewer (reused prebuilt WASM)
# =============================================================================
section "Preparing viewer (WebAssembly)..."
[[ "${SKIP_VIEWER_BUILD}" == "1" ]] || err "This script only supports SKIP_VIEWER_BUILD=1 (reuse prebuilt viewer)"
[[ -f "${VIEWER_BUILD}/OHTwinViewer.wasm" ]] || err "No prebuilt viewer at ${VIEWER_BUILD}/OHTwinViewer.wasm"

# =============================================================================
# Step 3 — Stage libs, wrapper, systemd unit, viewer config
# =============================================================================
section "Staging JM 3-cell truth artifacts..."
BUNDLE_DIR="/tmp/drywelldt_jm_3cell_truth_bundle"
rm -rf "${BUNDLE_DIR}"; mkdir -p "${BUNDLE_DIR}/lib" "${BUNDLE_DIR}/plugins/tls"

for lib in libQt6Network.so.6 libQt6Core.so.6 \
           libicui18n.so.73 libicuuc.so.73 libicudata.so.73; do
    [[ -f "${QT_LIB_DIR}/${lib}" ]] && cp "${QT_LIB_DIR}/${lib}" "${BUNDLE_DIR}/lib/" || warn "Qt lib not found: ${lib}"
done
[[ -d "${OHQ_LIB_DIR}" ]] && cp "${OHQ_LIB_DIR}"/*.so* "${BUNDLE_DIR}/lib/" 2>/dev/null || true

# Same EC2 armadillo/openblas/superlu version gaps as the other two forks --
# see deploy_jm_3cell.sh for the full story of how these were found (ldd only
# reveals a missing lib's OWN transitive deps once that lib itself is present,
# so this took two rounds to fully discover against the live server).
for pattern in 'libarmadillo\.so\.[0-9]* ' 'libopenblas\.so\.0 ' 'libsuperlu\.so\.5 '; do
    SO="$(ldconfig -p 2>/dev/null | grep "${pattern}" | head -1 | awk '{print $NF}')"
    [[ -n "${SO}" ]] || err "Pattern not found via ldconfig on this machine: ${pattern}"
    cp -L "${SO}" "${BUNDLE_DIR}/lib/$(basename "${SO}")"
    log "Bundling $(basename "${SO}")"
done

cp "${QT_DESKTOP}/plugins/tls/libqopensslbackend.so" "${BUNDLE_DIR}/plugins/tls/"

cat > "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh" << WRAPPER
#!/bin/bash
set -e
DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
DEPLOYMENT="\$1"
export LD_LIBRARY_PATH="\${DIR}/../lib:\${LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\${DIR}/../plugins"
exec "\${DIR}/OHTwin" --deployment "${EC2_DEPLOY_ROOT}/\${DEPLOYMENT}"
WRAPPER
chmod +x "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh"

# Same template the other two forks install -- idempotent to re-write identical content.
cat > "${BUNDLE_DIR}/${SERVICE_PREFIX}@.service" << UNIT
[Unit]
Description=DrywellDT Digital Twin - JM 3-cell (%i)
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=${EC2_BIN}
ExecStart=${EC2_BIN}/run_drywelldt_jm_3cell.sh %i
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=${SERVICE_PREFIX}-%i

[Install]
WantedBy=multi-user.target
UNIT

# Viewer config — same "forward" mode as forecast; truth is just another
# forward-mode page in Bioretention's three-page convention.
cat > "${BUNDLE_DIR}/config.json" << EOF
{
    "mode": "forward",
    "refresh_seconds": 300,
    "forward": {
        "csv_url":                "http://${DOMAIN}/${PAGE}/outputs/selected_output.csv",
        "viz_url":                "http://${DOMAIN}/${PAGE}/outputs/viz.svg",
        "viz_state_url":          "http://${DOMAIN}/${PAGE}/outputs/viz_state.json",
        "forecast_viz_url":       "http://${DOMAIN}/${PAGE}/outputs/forecast_viz.svg",
        "forecast_viz_state_url": "http://${DOMAIN}/${PAGE}/outputs/forecast_viz_state.json"
    }
}
EOF

# =============================================================================
# Step 4 — Prepare EC2
# =============================================================================
section "Preparing EC2 (jm_twin_3cell paths only)..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" "${SERVICE_PREFIX}" << 'ENDSSH'
set -e
D="$1"; SVC="$2"
mkdir -p /home/ubuntu/jm_twin_3cell/app/{bin,lib,plugins/tls}
mkdir -p /home/ubuntu/jm_twin_3cell/{resources,deployments}
sudo mkdir -p /var/www/drywelldt
sudo chown -R ubuntu:ubuntu /var/www/drywelldt
sudo mkdir -p /etc/nginx/ohtwin-locations
sudo systemctl stop "${SVC}@${D}.service" 2>/dev/null || true
ENDSSH

# =============================================================================
# Step 5 — Push binary/libs/resources (idempotent -- already there from the
# other two forks, but this works standalone too)
# =============================================================================
section "Pushing runner and libraries..."
# Atomic push -- forecast_3cell and assimilation_3cell share this same binary
# path and are running right now, so scp'ing directly into it fails with
# "Text file busy". Push aside, then mv into place (a same-filesystem rename,
# not a write -- safe even while the old inode is still mapped into those
# running processes). See deploy_jm_3cell.sh for the same fix.
scp "${SCP_OPTS[@]}" "${RUNNER_BIN}"                              "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/OHTwin.new"
ssh "${SSH_OPTS[@]}" "mv ${EC2_BIN}/OHTwin.new ${EC2_BIN}/OHTwin"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh"    "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/lib/*                        "${EC2_USER}@${EC2_HOST}:${EC2_LIB}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/plugins/tls/libqopensslbackend.so \
                                                                  "${EC2_USER}@${EC2_HOST}:${EC2_PLUGINS}/tls/"
scp "${SCP_OPTS[@]}" "${LOCAL_PROJECT}/resources"/*.json \
                     "${LOCAL_PROJECT}/resources"/*.list          "${EC2_USER}@${EC2_HOST}:${EC2_RESOURCES}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/${SERVICE_PREFIX}@.service"  "${EC2_USER}@${EC2_HOST}:/tmp/${SERVICE_PREFIX}@.service"
ssh "${SSH_OPTS[@]}" bash -s -- "${SERVICE_PREFIX}" << 'ENDSSH'
set -e
SVC="$1"
sudo mv "/tmp/${SVC}@.service" "/etc/systemd/system/${SVC}@.service"
sudo systemctl daemon-reload
chmod +x /home/ubuntu/jm_twin_3cell/app/bin/OHTwin
chmod +x /home/ubuntu/jm_twin_3cell/app/bin/run_drywelldt_jm_3cell.sh
ENDSSH

# =============================================================================
# Step 6 — Push the deployment directory
# =============================================================================
section "Rsyncing deployments/${DEPLOYMENT}..."
rsync -av --delete \
    --exclude='outputs/' --exclude='state/' --exclude='snapshots/' \
    -e "ssh -i \"${PEM_FILE}\"" \
    "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/" \
    "${EC2_USER}@${EC2_HOST}:${EC2_DEPLOY_ROOT}/${DEPLOYMENT}/"

ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" "${FRESH}" << 'ENDSSH'
set -e
D="$1"; FRESH="$2"
BASE="/home/ubuntu/jm_twin_3cell/deployments/${D}"
if [[ "$FRESH" == "1" ]]; then
    echo "[remote] --fresh: wiping outputs/state/snapshots for ${D} only"
    rm -rf "${BASE}/outputs" "${BASE}/state" "${BASE}/snapshots"
else
    echo "[remote] preserving existing outputs/state/snapshots"
fi
mkdir -p "${BASE}/outputs" "${BASE}/state" "${BASE}/snapshots"
chmod -R o+rx "${BASE}/outputs"
ENDSSH

# =============================================================================
# Step 7 — Viewer page
# =============================================================================
section "Publishing viewer page /${PAGE}/ ..."
VIEW_STAGE="${BUNDLE_DIR}/viewer"
rm -rf "${VIEW_STAGE}"; mkdir -p "${VIEW_STAGE}"
cp "${VIEWER_BUILD}/OHTwinViewer.html" \
   "${VIEWER_BUILD}/OHTwinViewer.js"   \
   "${VIEWER_BUILD}/OHTwinViewer.wasm" \
   "${VIEWER_BUILD}/qtloader.js"       \
   "${VIEWER_BUILD}/qtlogo.svg"        \
   "${VIEW_STAGE}/"
cp "${BUNDLE_DIR}/config.json" "${VIEW_STAGE}/"

SPLASH_FRAGMENT="${LOCAL_VIEWER}/splash_loading.html"
if [[ -f "${SPLASH_FRAGMENT}" ]]; then
    log "  Injecting loading splash..."
    python3 - "${VIEW_STAGE}/OHTwinViewer.html" "${SPLASH_FRAGMENT}" << 'PYEOF'
import re, sys
page_path, frag_path = sys.argv[1], sys.argv[2]
page = open(page_path).read()
frag = open(frag_path).read()
m = re.search(r'<figure[^>]*id="qtspinner".*?</figure>', page, re.S)
if not m:
    sys.exit("could not find the #qtspinner block in %s" % page_path)
page = page[:m.start()] + frag + page[m.end():]
page = page.replace(
    "const status = document.querySelector('#qtstatus');",
    "const status = document.querySelector('#qtstatus') || { set innerHTML(v) {\n"
    "                var s = document.getElementById('dt-status');\n"
    "                if (s) s.textContent = String(v).replace(/<[^>]*>/g, '');\n"
    "            }, get innerHTML() { return ''; } };")
old_showui = """            const showUi = (ui) => {
                [spinner, screen].forEach(element => element.style.display = 'none');
                if (screen === ui)
                    screen.style.position = 'default';
                ui.style.display = 'block';
            }"""
new_showui = """            const showUi = (ui) => {
                screen.style.display = 'block';
                spinner.style.display = (ui === spinner) ? 'block' : 'none';
            }"""
if old_showui not in page:
    sys.exit("showUi() is not in the expected form in %s" % page_path)
page = page.replace(old_showui, new_showui, 1)
page = page.replace(
    "#screen { width: 100%; height: 100%; }",
    "#screen { width: 100%; height: 100%; }\n"
    "      #qtspinner { position: fixed; inset: 0; z-index: 9999; margin: 0; }", 1)
page = page.replace("onLoaded: () => showUi(screen),",
                    "onLoaded: () => { if (window.dtSplash) window.dtSplash.done();\n"
                    "                                      setTimeout(() => showUi(screen), 600); },")
page = page.replace("showUi(spinner);\n                            status.innerHTML",
                    "if (window.dtSplash) window.dtSplash.reset();\n"
                    "                            showUi(spinner);\n                            status.innerHTML")
open(page_path, "w").write(page)
print("    splash injected (%d bytes)" % len(frag))
PYEOF
else
    warn "  No splash fragment at ${SPLASH_FRAGMENT} — keeping the stock Qt logo screen"
fi

ssh "${SSH_OPTS[@]}" "mkdir -p ${EC2_WWW}/${PAGE} && rm -f ${EC2_WWW}/${PAGE}/*"
scp "${SCP_OPTS[@]}" "${VIEW_STAGE}"/* "${EC2_USER}@${EC2_HOST}:${EC2_WWW}/${PAGE}/"

# =============================================================================
# Step 8 — nginx (additive, validated, rolled back on failure)
# =============================================================================
section "Installing nginx location block for /${PAGE}/ ..."
ssh "${SSH_OPTS[@]}" bash -s -- \
    "${DEPLOYMENT}" "${PAGE}" "${SET_LANDING}" "${NGINX_SITE}" "${NGINX_LOC_DIR}" << 'ENDSSH'
set -e
D="$1"; PAGE="$2"; SET_LANDING="$3"; SITE="$4"; LOCDIR="$5"
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="/etc/nginx/sites-available/openhydrotwin.bak-${STAMP}"

sudo mkdir -p "${LOCDIR}"

sudo tee "${LOCDIR}/${D}.conf" > /dev/null << EOF
# ${D} — truth/reference viewer, 3-cell corrected model. Generated by deploy_jm_truth_3cell.sh.
location /${PAGE}/outputs/ {
    alias /home/ubuntu/jm_twin_3cell/deployments/${D}/outputs/;
    add_header Access-Control-Allow-Origin * always;
    add_header Cache-Control no-cache always;
    add_header Cross-Origin-Resource-Policy cross-origin always;
    add_header Cross-Origin-Opener-Policy same-origin always;
    add_header Cross-Origin-Embedder-Policy require-corp always;
}
location /${PAGE}/ {
    alias /var/www/drywelldt/${PAGE}/;
    index OHTwinViewer.html;
    try_files \$uri \$uri/ /${PAGE}/OHTwinViewer.html;
    add_header Cross-Origin-Opener-Policy same-origin always;
    add_header Cross-Origin-Embedder-Policy require-corp always;
}
EOF

NEED_RELOAD=0
if ! grep -q "ohtwin-locations" "${SITE}"; then
    echo "[remote] adding include line to ${SITE} (backup: ${BACKUP})"
    sudo cp "${SITE}" "${BACKUP}"
    sudo python3 - "${SITE}" "${LOCDIR}" << 'PYEOF'
import sys
path, locdir = sys.argv[1], sys.argv[2]
lines = open(path).read().rstrip("\n").split("\n")
for i in range(len(lines) - 1, -1, -1):
    if lines[i].strip() == "}":
        lines.insert(i, "")
        lines.insert(i + 1, "    # Per-deployment location blocks (deploy_jm.sh and friends)")
        lines.insert(i + 2, "    include %s/*.conf;" % locdir)
        break
else:
    raise SystemExit("could not find closing brace in %s" % path)
open(path, "w").write("\n".join(lines) + "\n")
PYEOF
    NEED_RELOAD=1
fi

if [[ "${SET_LANDING}" == "1" ]]; then
    if ! grep -q "return 302 /${PAGE}/;" "${SITE}"; then
        echo "[remote] repointing site root to /${PAGE}/"
        [[ -f "${BACKUP}" ]] || sudo cp "${SITE}" "${BACKUP}"
        sudo sed -i "s|return 302 /Bioretention_assimilation/;|return 302 /${PAGE}/;|" "${SITE}"
        NEED_RELOAD=1
    fi
fi

if ! sudo nginx -t 2>/tmp/nginx_test.err; then
    echo "[remote] nginx -t FAILED:"; cat /tmp/nginx_test.err
    sudo rm -f "${LOCDIR}/${D}.conf"
    if [[ -f "${BACKUP}" ]]; then
        echo "[remote] restoring ${SITE} from ${BACKUP}"
        sudo cp "${BACKUP}" "${SITE}"
    fi
    sudo nginx -t && sudo systemctl reload nginx
    exit 1
fi
sudo systemctl reload nginx
echo "[remote] nginx reloaded OK"
ENDSSH

# =============================================================================
# Step 9 — Start this instance of the shared template
# =============================================================================
section "Starting ${SERVICE_PREFIX}@${DEPLOYMENT} ..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" "${SERVICE_PREFIX}" << 'ENDSSH'
set -e
D="$1"; SVC="$2"
sudo systemctl enable "${SVC}@${D}.service"
sudo systemctl restart "${SVC}@${D}.service"
sleep 5
sudo systemctl status "${SVC}@${D}.service" --no-pager | head -18
echo
echo "----- All jm_3cell instances (forecast + assimilation + truth share this template) -----"
systemctl list-units "${SVC}@*" --no-pager --no-legend
echo
echo "----- Bioretention services (must still be running, untouched) -----"
systemctl list-units "drywelldt@*" --no-pager --no-legend
echo
echo "----- Original JM forecast (must still be running, untouched) -----"
systemctl list-units "drywelldt-jm@*" --no-pager --no-legend
ENDSSH

# =============================================================================
# Done
# =============================================================================
section "JM 3-cell truth deployment complete"
echo
echo "  Viewer:  http://${DOMAIN}/${PAGE}/"
echo "  Data:    http://${DOMAIN}/${PAGE}/outputs/"
echo
echo "  Logs:"
echo "    ssh -i \"${PEM_FILE}\" ${EC2_USER}@${EC2_HOST}"
echo "    sudo journalctl -u ${SERVICE_PREFIX}@${DEPLOYMENT} -f"
