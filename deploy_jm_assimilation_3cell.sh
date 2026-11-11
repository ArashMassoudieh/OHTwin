#!/bin/bash
# =============================================================================
# JM 3-cell assimilation + live sensor bridge — AWS Deploy
#
# Companion to deploy_jm_3cell.sh (which deploys the forecast/review page).
# This deploys two things onto the SAME jm_twin_3cell tree and systemd
# template that deploy_jm_3cell.sh set up:
#
#   1. JM_assimilation_3cell itself, as another instance of the
#      drywelldt-jm-3cell@ template (drywelldt-jm-3cell@JM_assimilation_3cell),
#      sharing the already-pushed binary/resources -- this script re-pushes
#      them too, so it also works if run on its own.
#
#   2. The sensor bridge (deployments/JM_assimilation_3cell/bridge/sensor_bridge.py),
#      as its own small standalone systemd service, jm-sensor-bridge-3cell,
#      bound to 127.0.0.1:8198. It polls the REAL field API
#      (http://54.213.147.59:5000, project John-McCormack) and republishes
#      Pond 1 water depth / Soil 1 moisture for DTAssimilation to fetch from
#      localhost -- same mechanism verified locally via an SSH tunnel.
#
# Why the bridge needs its own nginx location (new pattern here)
#   Every other location block in this codebase serves static files that
#   OHTwin already wrote to outputs/, via `alias`. The bridge computes its
#   response fresh on each request (no file on disk), so the browser-facing
#   viewer needs `proxy_pass` to 127.0.0.1:8198 instead. The assimilation
#   ENGINE itself (DTAssimilation, running on this same server) talks to the
#   bridge directly over localhost:8198 and needs no nginx involvement at all
#   -- only the browser-facing viewer's observed_csv_url needs the proxy.
#
# Isolation -- same guarantees as deploy_jm_3cell.sh: no shared path, systemd
# unit, or file with Bioretention, the original JM_forecast, or GreenInfraIQ.
#   assimilation unit   drywelldt-jm-3cell@JM_assimilation_3cell  (shared TEMPLATE
#                        with forecast_3cell, by design -- same pattern Bioretention
#                        uses for its own truth/assimilation pair)
#   bridge unit          jm-sensor-bridge-3cell.service  (standalone, new)
#   bridge files         /home/ubuntu/jm_sensor_bridge_3cell/  (own tree)
#   nginx                /etc/nginx/ohtwin-locations/JM_assimilation_3cell.conf
#                        /etc/nginx/ohtwin-locations/JM_sensor_bridge_3cell.conf
#   page/path            /JM_Bioretention_assimilation_3cell/
#                        /JM_sensor_bridge_3cell/  (proxy only, not a viewer page)
#
# Usage:
#   ./deploy_jm_assimilation_3cell.sh [--fresh]
#   --fresh  Wipe outputs/, state/, snapshots/ for JM_assimilation_3cell only.
# =============================================================================

set -e

EC2_USER="ubuntu"
EC2_HOST="52.42.223.42"
DOMAIN="openhydrotwin.com"
PEM_FILE="${PEM_FILE:-/home/arash/.ssh/ArashLinux.pem}"

LOCAL_PROJECT="/mnt/3rd900/Projects/OHTwin"
LOCAL_VIEWER="${LOCAL_PROJECT}/viewer"

DEPLOYMENT="JM_assimilation_3cell"
PAGE="JM_Bioretention_assimilation_3cell"
SERVICE_PREFIX="drywelldt-jm-3cell"        # shared template with deploy_jm_3cell.sh

BRIDGE_SERVICE="jm-sensor-bridge-3cell"     # standalone, not templated
BRIDGE_PORT=8198
BRIDGE_PAGE="JM_sensor_bridge_3cell"        # proxy-only path, not a viewer
BRIDGE_API="http://54.213.147.59:5000"
BRIDGE_PROJECT="John-McCormack"
BRIDGE_WINDOW_DAYS=14
EC2_BRIDGE_ROOT="/home/ubuntu/jm_sensor_bridge_3cell"

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

EC2_ROOT="/home/ubuntu/jm_twin_3cell"       # same tree deploy_jm_3cell.sh uses
EC2_APP="${EC2_ROOT}/app"
EC2_BIN="${EC2_APP}/bin"
EC2_LIB="${EC2_APP}/lib"
EC2_PLUGINS="${EC2_APP}/plugins"
EC2_RESOURCES="${EC2_ROOT}/resources"
EC2_DEPLOY_ROOT="${EC2_ROOT}/deployments"
EC2_WWW="/var/www/drywelldt"
NGINX_LOC_DIR="/etc/nginx/ohtwin-locations"
NGINX_SITE="/etc/nginx/sites-available/openhydrotwin"

FRESH=0
for arg in "$@"; do
    case "$arg" in
        --fresh) FRESH=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

SSH_OPTS=(-i "${PEM_FILE}" "${EC2_USER}@${EC2_HOST}")
SCP_OPTS=(-i "${PEM_FILE}")
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()    { echo -e "${GREEN}[jm3c-assim]${NC} $1"; }
warn()   { echo -e "${YELLOW}[jm3c-assim]${NC} $1"; }
err()    { echo -e "${RED}[jm3c-assim]${NC} $1"; exit 1; }
section(){ echo -e "${GREEN}[jm3c-assim]${NC} =================================================="; \
           echo -e "${GREEN}[jm3c-assim]${NC} $1"; \
           echo -e "${GREEN}[jm3c-assim]${NC} =================================================="; }

# =============================================================================
# Step 0 — Pre-flight
# =============================================================================
section "JM 3-cell assimilation deploy — ${DEPLOYMENT} -> /${PAGE}/  (fresh=${FRESH})"

[[ -f "${PEM_FILE}" ]] || err "SSH key not found: ${PEM_FILE}"
[[ -d "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}" ]] || err "Local deployment not found: deployments/${DEPLOYMENT}"
BRIDGE_SCRIPT="${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/bridge/sensor_bridge.py"
[[ -f "${BRIDGE_SCRIPT}" ]] || err "Bridge script not found: ${BRIDGE_SCRIPT}"
[[ -x "${QMAKE_DESKTOP}" ]] || err "qmake not found at ${QMAKE_DESKTOP}"

if python3 - "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" "${DEPLOYMENT}" << 'PYCHK'
import json, sys
d = json.load(open(sys.argv[1]))
expected_name = sys.argv[2]
bad = []
if "assimilation" not in d:
    bad.append("has no 'assimilation' section (this IS an assimilation deployment)")
if d["deployment"]["name"] != expected_name:
    bad.append("deployment.name is %r, expected %r" % (d["deployment"]["name"], expected_name))
a = d.get("assimilation", {})
if "localhost:8198" not in a.get("truth_csv_url", ""):
    bad.append("truth_csv_url %r doesn't point at localhost:8198 -- "
                "this script deploys the bridge to that port on THIS server; "
                "a different port here would silently talk to nothing" % a.get("truth_csv_url"))
for b in bad:
    print(b)
sys.exit(1 if bad else 0)
PYCHK
then :; else err "config.json sanity check failed (see above)"; fi
log "config.json sanity check passed"

# =============================================================================
# Step 1 — Build runner (reuses deploy_jm_3cell.sh's build; cheap no-op if
# already built and sources haven't changed)
# =============================================================================
section "Building OHTwin runner (Release, Qt ${QT_VER})..."
mkdir -p "${RUNNER_BUILD}"
cd "${RUNNER_BUILD}"
"${QMAKE_DESKTOP}" "${PRO_FILE}" CONFIG+=release CONFIG-=debug
make -j"$(nproc)"
[[ -x "${RUNNER_BIN}" ]] || err "Build produced no binary at ${RUNNER_BIN}"
log "Runner: ${RUNNER_BIN}"

# =============================================================================
# Step 2 — Viewer (reused prebuilt WASM, same as deploy_jm_3cell.sh)
# =============================================================================
section "Preparing viewer (WebAssembly)..."
[[ "${SKIP_VIEWER_BUILD}" == "1" ]] || err "This script only supports SKIP_VIEWER_BUILD=1 (reuse prebuilt viewer)"
[[ -f "${VIEWER_BUILD}/OHTwinViewer.wasm" ]] || err "No prebuilt viewer at ${VIEWER_BUILD}/OHTwinViewer.wasm"

# =============================================================================
# Step 3 — Stage artifacts
# =============================================================================
section "Staging assimilation + bridge artifacts..."
BUNDLE_DIR="/tmp/drywelldt_jm_3cell_assim_bundle"
rm -rf "${BUNDLE_DIR}"; mkdir -p "${BUNDLE_DIR}/lib" "${BUNDLE_DIR}/plugins/tls"

for lib in libQt6Network.so.6 libQt6Core.so.6 \
           libicui18n.so.73 libicuuc.so.73 libicudata.so.73; do
    [[ -f "${QT_LIB_DIR}/${lib}" ]] && cp "${QT_LIB_DIR}/${lib}" "${BUNDLE_DIR}/lib/" || warn "Qt lib not found: ${lib}"
done
[[ -d "${OHQ_LIB_DIR}" ]] && cp "${OHQ_LIB_DIR}"/*.so* "${BUNDLE_DIR}/lib/" 2>/dev/null || true

# This dev machine's libarmadillo (14.x) is newer than what EC2 has
# system-wide (libarmadillo12) -- confirmed via ldd on the pushed binary that
# this is the ONLY missing dependency (BLAS/LAPACK/GSL/SuperLU etc. are
# already satisfied by EC2's existing install). See deploy_jm_3cell.sh for
# the same fix on the forecast side.
ARMADILLO_SO="$(ldconfig -p 2>/dev/null | grep 'libarmadillo\.so\.[0-9]* ' | head -1 | awk '{print $NF}')"
[[ -n "${ARMADILLO_SO}" ]] || err "libarmadillo.so.* not found via ldconfig on this machine"
cp -L "${ARMADILLO_SO}" "${BUNDLE_DIR}/lib/$(basename "${ARMADILLO_SO}")"
log "Bundling $(basename "${ARMADILLO_SO}") (not present on EC2 at this version)"

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

# Same template deploy_jm_3cell.sh installs -- idempotent to re-write identical content.
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

# Bridge's own standalone unit -- not a template, exactly one instance.
cat > "${BUNDLE_DIR}/${BRIDGE_SERVICE}.service" << UNIT
[Unit]
Description=AnacostiaIQ field sensor bridge for JM_assimilation_3cell (John-McCormack)
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=${EC2_BRIDGE_ROOT}
ExecStart=/usr/bin/python3 ${EC2_BRIDGE_ROOT}/sensor_bridge.py --port ${BRIDGE_PORT} --api ${BRIDGE_API} --project ${BRIDGE_PROJECT} --window-days ${BRIDGE_WINDOW_DAYS}
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=${BRIDGE_SERVICE}

[Install]
WantedBy=multi-user.target
UNIT

# Assimilation viewer config — observed data comes from the bridge's proxied
# path (browser-reachable), modeled/ga from this deployment's own page.
cat > "${BUNDLE_DIR}/config.json" << EOF
{
    "mode": "assimilation",
    "refresh_seconds": 300,
    "assimilation": {
        "ga_merged_url":    "http://${DOMAIN}/${PAGE}/outputs/calibration/ga_output_merged.txt",
        "observed_csv_url": "http://${DOMAIN}/${BRIDGE_PAGE}/outputs/selected_output.csv",
        "modeled_csv_url":  "http://${DOMAIN}/${PAGE}/outputs/selected_output.csv",
        "x_axis": "t_now",
        "fitness_metrics": ["nse", "nmse"],
        "parameter_panel": {
            "show_population_band": true
        }
    }
}
EOF

# =============================================================================
# Step 4 — Prepare EC2
# =============================================================================
section "Preparing EC2 (jm_twin_3cell + bridge paths only)..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" "${SERVICE_PREFIX}" "${BRIDGE_SERVICE}" "${EC2_BRIDGE_ROOT}" << 'ENDSSH'
set -e
D="$1"; SVC="$2"; BRIDGE_SVC="$3"; BRIDGE_ROOT="$4"
mkdir -p /home/ubuntu/jm_twin_3cell/app/{bin,lib,plugins/tls}
mkdir -p /home/ubuntu/jm_twin_3cell/{resources,deployments}
mkdir -p "${BRIDGE_ROOT}"
sudo mkdir -p /var/www/drywelldt
sudo chown -R ubuntu:ubuntu /var/www/drywelldt
sudo mkdir -p /etc/nginx/ohtwin-locations
sudo systemctl stop "${SVC}@${D}.service" 2>/dev/null || true
sudo systemctl stop "${BRIDGE_SVC}.service" 2>/dev/null || true
ENDSSH

# =============================================================================
# Step 5 — Push binary/libs/resources (idempotent) + the bridge script
# =============================================================================
section "Pushing runner, libraries, and sensor bridge..."
scp "${SCP_OPTS[@]}" "${RUNNER_BIN}"                           "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh" "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/lib/*                     "${EC2_USER}@${EC2_HOST}:${EC2_LIB}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/plugins/tls/libqopensslbackend.so \
                                                               "${EC2_USER}@${EC2_HOST}:${EC2_PLUGINS}/tls/"
scp "${SCP_OPTS[@]}" "${LOCAL_PROJECT}/resources"/*.json \
                     "${LOCAL_PROJECT}/resources"/*.list       "${EC2_USER}@${EC2_HOST}:${EC2_RESOURCES}/"
scp "${SCP_OPTS[@]}" "${BRIDGE_SCRIPT}"                        "${EC2_USER}@${EC2_HOST}:${EC2_BRIDGE_ROOT}/sensor_bridge.py"

scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/${SERVICE_PREFIX}@.service"  "${EC2_USER}@${EC2_HOST}:/tmp/${SERVICE_PREFIX}@.service"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/${BRIDGE_SERVICE}.service"    "${EC2_USER}@${EC2_HOST}:/tmp/${BRIDGE_SERVICE}.service"
ssh "${SSH_OPTS[@]}" bash -s -- "${SERVICE_PREFIX}" "${BRIDGE_SERVICE}" << 'ENDSSH'
set -e
SVC="$1"; BRIDGE_SVC="$2"
sudo mv "/tmp/${SVC}@.service" "/etc/systemd/system/${SVC}@.service"
sudo mv "/tmp/${BRIDGE_SVC}.service" "/etc/systemd/system/${BRIDGE_SVC}.service"
sudo systemctl daemon-reload
chmod +x /home/ubuntu/jm_twin_3cell/app/bin/OHTwin
chmod +x /home/ubuntu/jm_twin_3cell/app/bin/run_drywelldt_jm_3cell.sh
command -v python3 >/dev/null || { echo "[remote] python3 not found" >&2; exit 1; }
ENDSSH

# =============================================================================
# Step 6 — Push the deployment directory
# =============================================================================
section "Rsyncing deployments/${DEPLOYMENT}..."
rsync -av --delete \
    --exclude='outputs/' --exclude='state/' --exclude='snapshots/' --exclude='bridge/' \
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
mkdir -p "${BASE}/outputs" "${BASE}/state" "${BASE}/snapshots" "${BASE}/outputs/calibration"
chmod -R o+rx "${BASE}/outputs"
ENDSSH

# =============================================================================
# Step 7 — Viewer page (AssimViewer config; reuses the already-published WASM
# viewer bundle from deploy_jm_3cell.sh if present, publishes its own copy
# with assimilation-mode config.json either way)
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
# Step 8 — nginx: assimilation page (alias, same pattern as everything else)
#                  + bridge proxy (new pattern, proxy_pass -- see header note)
# =============================================================================
section "Installing nginx location blocks..."
ssh "${SSH_OPTS[@]}" bash -s -- \
    "${DEPLOYMENT}" "${PAGE}" "${BRIDGE_PAGE}" "${BRIDGE_PORT}" "${NGINX_SITE}" "${NGINX_LOC_DIR}" << 'ENDSSH'
set -e
D="$1"; PAGE="$2"; BPAGE="$3"; BPORT="$4"; SITE="$5"; LOCDIR="$6"
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="/etc/nginx/sites-available/openhydrotwin.bak-${STAMP}"

sudo mkdir -p "${LOCDIR}"

sudo tee "${LOCDIR}/${D}.conf" > /dev/null << EOF
# ${D} — assimilation viewer, 3-cell corrected model. Generated by deploy_jm_assimilation_3cell.sh.
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

sudo tee "${LOCDIR}/${BPAGE}.conf" > /dev/null << EOF
# ${BPAGE} — live field sensor bridge, proxied (not static like everything
# else here: the bridge computes its response fresh per request, there's no
# file on disk to alias). Generated by deploy_jm_assimilation_3cell.sh.
location /${BPAGE}/outputs/ {
    proxy_pass http://127.0.0.1:${BPORT}/outputs/;
    add_header Access-Control-Allow-Origin * always;
    add_header Cache-Control no-cache always;
    add_header Cross-Origin-Resource-Policy cross-origin always;
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

if ! sudo nginx -t 2>/tmp/nginx_test.err; then
    echo "[remote] nginx -t FAILED:"; cat /tmp/nginx_test.err
    sudo rm -f "${LOCDIR}/${D}.conf" "${LOCDIR}/${BPAGE}.conf"
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
# Step 9 — Start the bridge, then the assimilation instance (bridge must be
# up first so the engine's initial refresh on startup has something to fetch)
# =============================================================================
section "Starting ${BRIDGE_SERVICE} then ${SERVICE_PREFIX}@${DEPLOYMENT} ..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" "${SERVICE_PREFIX}" "${BRIDGE_SERVICE}" << 'ENDSSH'
set -e
D="$1"; SVC="$2"; BRIDGE_SVC="$3"
sudo systemctl enable "${BRIDGE_SVC}.service"
sudo systemctl restart "${BRIDGE_SVC}.service"
sleep 3
sudo systemctl status "${BRIDGE_SVC}.service" --no-pager | head -12
echo
sudo systemctl enable "${SVC}@${D}.service"
sudo systemctl restart "${SVC}@${D}.service"
sleep 5
sudo systemctl status "${SVC}@${D}.service" --no-pager | head -18
echo
echo "----- Bioretention services (must still be running, untouched) -----"
systemctl list-units "drywelldt@*" --no-pager --no-legend
echo
echo "----- Original JM forecast (must still be running, untouched) -----"
systemctl list-units "drywelldt-jm@*" --no-pager --no-legend
echo
echo "----- All jm_3cell instances (forecast + assimilation share this template) -----"
systemctl list-units "${SVC}@*" --no-pager --no-legend
ENDSSH

# =============================================================================
# Done
# =============================================================================
section "JM 3-cell assimilation deployment complete"
echo
echo "  Viewer:  http://${DOMAIN}/${PAGE}/"
echo "  Data:    http://${DOMAIN}/${PAGE}/outputs/"
echo "  Bridge:  http://${DOMAIN}/${BRIDGE_PAGE}/outputs/selected_output.csv"
echo
echo "  Logs:"
echo "    ssh -i \"${PEM_FILE}\" ${EC2_USER}@${EC2_HOST}"
echo "    sudo journalctl -u ${SERVICE_PREFIX}@${DEPLOYMENT} -f"
echo "    sudo journalctl -u ${BRIDGE_SERVICE} -f"
