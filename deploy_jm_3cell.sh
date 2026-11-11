#!/bin/bash
# =============================================================================
# JM (John McCormack Rd) Bioretention — 3-cell corrected forecast — AWS Deploy
#
# Forked from deploy_jm.sh. The site has only 3 constructed cells (confirmed
# on site; the 4th was never built), and JM_forecast_3cell/JM.ohq has the
# geometry, link, and observation corrections for that -- see
# deployments/JM_forecast_3cell/model/JM.ohq's header comment for the full
# diff against the original 4-column model.
#
# Usage:
#   ./deploy_jm_3cell.sh [--fresh] [--set-landing]
#
#   --fresh        Wipe outputs/, state/ and snapshots/ on EC2 before starting.
#   --set-landing  Point openhydrotwin.com/ at this page. NOT passed by
#                  default -- this is a review copy for Arash, the live
#                  JM_Bioretention_forecast/ page stays the landing target
#                  until a deliberate decision to replace it.
#
# Isolation -- from BOTH the live Bioretention services AND the original
# (uncorrected) JM_forecast, not just one or the other. This script shares
# NO path, systemd unit, or file with either:
#   everything    /home/ubuntu/jm_twin_3cell/    (its own tree, not jm_twin/)
#     binary        jm_twin_3cell/app/bin/OHTwin
#     libs          jm_twin_3cell/app/lib/
#     templates     jm_twin_3cell/resources/
#     deployments   jm_twin_3cell/deployments/
#   systemd unit  drywelldt-jm-3cell@.service   (NOT drywelldt@, NOT drywelldt-jm@)
#   nginx         /etc/nginx/ohtwin-locations/JM_forecast_3cell.conf
#   page/path     /JM_Bioretention_forecast_3cell/  (NOT /JM_Bioretention_forecast/)
# Writing a shared systemd TEMPLATE file (drywelldt-jm@.service) would have
# repointed every instance of that template -- including the live
# drywelldt-jm@JM_forecast.service -- which is exactly the kind of in-place
# disruption to an existing deployment this fork exists to avoid.
#
# Addressing
#   page   http://openhydrotwin.com/JM_Bioretention_forecast_3cell/
#   data   http://openhydrotwin.com/JM_Bioretention_forecast_3cell/outputs/...
#   Path-based on port 80, same routing style as the other pages. deployment.port
#   in config.json is metadata only -- DTConfig validates it is positive but
#   nothing binds it (DTConfig.cpp:169); nginx serves outputs/ via `alias`.
#
# nginx integration
#   Shared server block with Bioretention and the original JM. This script
#   only ever writes its own file in /etc/nginx/ohtwin-locations/, named after
#   DEPLOYMENT (JM_forecast_3cell.conf), which cannot collide with
#   JM_forecast.conf or any Bioretention_*.conf. Every nginx change is
#   validated with `nginx -t` and rolled back on failure.
# =============================================================================

set -e

# --- Configuration ---------------------------------------------------------
EC2_USER="ubuntu"
EC2_PUBLIC_IP="52.42.223.42"
EC2_HOST="${EC2_PUBLIC_IP}"
DOMAIN="openhydrotwin.com"

PEM_FILE="${PEM_FILE:-/home/arash/.ssh/ArashLinux.pem}"

# Fixed from deploy_jm.sh's stale /home/arash/Projects/OpenHydroTwin, which
# does not exist on this machine -- the repo lives here instead.
LOCAL_PROJECT="/mnt/3rd900/Projects/OHTwin"
LOCAL_VIEWER="${LOCAL_PROJECT}/viewer"

DEPLOYMENT="JM_forecast_3cell"          # directory under deployments/
PAGE="JM_Bioretention_forecast_3cell"   # URL path and /var/www subdirectory
SERVICE_PREFIX="drywelldt-jm-3cell"     # systemd template name, own namespace

QT_VER="${QT_VER:-6.9.0}"
QT_ROOT="/home/arash/Qt/${QT_VER}"
QT_DESKTOP="${QT_ROOT}/gcc_64"
QMAKE_DESKTOP="${QT_DESKTOP}/bin/qmake"
QT_LIB_DIR="${QT_DESKTOP}/lib"
OHQ_LIB_DIR="${LOCAL_PROJECT}/libs/release"

# No emsdk/wasm toolchain on this machine -- always reuse the prebuilt viewer.
EMSDK_ENV="/home/arash/emsdk/emsdk_env.sh"
SKIP_VIEWER_BUILD="${SKIP_VIEWER_BUILD:-1}"
VIEWER_BUILD="${LOCAL_PROJECT}/build-wasm-viewer"

PRO_FILE="${LOCAL_PROJECT}/OHTwin.pro"
RUNNER_BINARY_NAME="OHTwin"
RUNNER_BUILD="${LOCAL_PROJECT}/build-qmake"
RUNNER_BIN="${RUNNER_BUILD}/bin/${RUNNER_BINARY_NAME}"

# EC2 layout — a third, entirely separate tree: not /home/ubuntu/drywelldt
# (Bioretention) and not /home/ubuntu/jm_twin (the original JM). Same
# ../../resources nesting-depth rule applies (DTRunner.cpp:855 resolves
# templates from applicationDirPath() + "/../../resources/"), so this own
# two-level-deep bin/ gives it its own template set too.
EC2_ROOT="/home/ubuntu/jm_twin_3cell"
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
        -h|--help)     sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Convenience -----------------------------------------------------------
SSH_OPTS=(-i "${PEM_FILE}" "${EC2_USER}@${EC2_HOST}")
SCP_OPTS=(-i "${PEM_FILE}")

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()    { echo -e "${GREEN}[jm3c]${NC} $1"; }
warn()   { echo -e "${YELLOW}[jm3c]${NC} $1"; }
err()    { echo -e "${RED}[jm3c]${NC} $1"; exit 1; }
section(){ echo -e "${GREEN}[jm3c]${NC} =================================================="; \
           echo -e "${GREEN}[jm3c]${NC} $1"; \
           echo -e "${GREEN}[jm3c]${NC} =================================================="; }

# =============================================================================
# Step 0 — Pre-flight
# =============================================================================
section "JM 3-cell deploy — ${DEPLOYMENT} -> /${PAGE}/  (fresh=${FRESH})"

[[ -f "${PEM_FILE}" ]] || err "SSH key not found: ${PEM_FILE}"
[[ -d "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}" ]] || \
    err "Local deployment not found: deployments/${DEPLOYMENT}"
[[ -f "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" ]] || \
    err "No config.json in deployments/${DEPLOYMENT}"
[[ -x "${QMAKE_DESKTOP}" ]] || \
    err "qmake not found at ${QMAKE_DESKTOP}. Set QT_VER to an installed Qt (have: $(ls -d /home/arash/Qt/6.* 2>/dev/null | xargs -n1 basename | paste -sd' '))."

# Refuse to run if the deployment still carries assimilation config or noise,
# or if it isn't actually this fork's own deployment.
if python3 - "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" "${DEPLOYMENT}" << 'PYCHK'
import json, sys
d = json.load(open(sys.argv[1]))
expected_name = sys.argv[2]
bad = []
if "assimilation" in d:
    bad.append("has an 'assimilation' section (this is a forecast-only deployment)")
ns = d.get("observations", {}).get("noise_sigma", {})
nz = {k: v for k, v in ns.items() if v}
if nz:
    bad.append("has non-zero noise_sigma: %s" % ", ".join(sorted(nz)))
if d["deployment"]["name"] != expected_name:
    bad.append("deployment.name is %r, expected %r" % (d["deployment"]["name"], expected_name))
for b in bad:
    print(b)
sys.exit(1 if bad else 0)
PYCHK
then :; else err "config.json sanity check failed (see above)"; fi
log "config.json sanity check passed"

# =============================================================================
# Step 1 — Build runner (Release, desktop) — same approach as deploy.sh
# =============================================================================
section "Building OHTwin runner (Release, Qt ${QT_VER})..."
mkdir -p "${RUNNER_BUILD}"
cd "${RUNNER_BUILD}"
"${QMAKE_DESKTOP}" "${PRO_FILE}" CONFIG+=release CONFIG-=debug
make -j"$(nproc)"
[[ -x "${RUNNER_BIN}" ]] || err "Build produced no binary at ${RUNNER_BIN}"

STALE_SRC="$(find "${LOCAL_PROJECT}" "${LOCAL_PROJECT}/../OpenHydroQual/aquifolium" \
        \( -path '*/build*' -o -path '*/.git' -o -path '*/deployments' \) -prune -o \
        -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -newer "${RUNNER_BIN}" -print 2>/dev/null | head -3)"
if [[ -n "${STALE_SRC}" ]]; then
    warn "These sources are newer than the binary — the build may be incomplete:"
    echo "${STALE_SRC}" | sed 's/^/          /'
fi
log "Runner: ${RUNNER_BIN} ($(du -h "${RUNNER_BIN}" | cut -f1), built $(date -r "${RUNNER_BIN}" '+%Y-%m-%d %H:%M:%S'))"

# =============================================================================
# Step 2 — Viewer (WebAssembly Release)
# =============================================================================
section "Preparing viewer (WebAssembly Release)..."
if [[ "${SKIP_VIEWER_BUILD}" == "1" ]]; then
    warn "SKIP_VIEWER_BUILD=1 — reusing existing viewer in ${VIEWER_BUILD}"
    [[ -f "${VIEWER_BUILD}/OHTwinViewer.wasm" ]] || \
        err "No prebuilt viewer at ${VIEWER_BUILD}/OHTwinViewer.wasm"
else
    [[ -f "${EMSDK_ENV}" ]] || err "emsdk env not found at ${EMSDK_ENV}; run with SKIP_VIEWER_BUILD=1"
    set +e; source "${EMSDK_ENV}" >/dev/null 2>&1; set -e
    command -v em++ >/dev/null 2>&1 || err "em++ not on PATH after sourcing emsdk"
    mkdir -p "${VIEWER_BUILD}"; cd "${VIEWER_BUILD}"
    "${QT_ROOT}/wasm_singlethread/bin/qmake" "${LOCAL_VIEWER}/OHTwinViewer.pro" \
        CONFIG+=release CONFIG-=debug
    make -j"$(nproc)"
    log "Viewer built."
fi

# =============================================================================
# Step 3 — Stage libs, wrapper, systemd unit, viewer config
# =============================================================================
section "Staging JM 3-cell artifacts..."
BUNDLE_DIR="/tmp/drywelldt_jm_3cell_bundle"
rm -rf "${BUNDLE_DIR}"; mkdir -p "${BUNDLE_DIR}/lib" "${BUNDLE_DIR}/plugins/tls"

for lib in libQt6Network.so.6 libQt6Core.so.6 \
           libicui18n.so.73 libicuuc.so.73 libicudata.so.73; do
    if [[ -f "${QT_LIB_DIR}/${lib}" ]]; then
        cp "${QT_LIB_DIR}/${lib}" "${BUNDLE_DIR}/lib/"
    else
        warn "Qt lib not found: ${lib}"
    fi
done
[[ -d "${OHQ_LIB_DIR}" ]] && cp "${OHQ_LIB_DIR}"/*.so* "${BUNDLE_DIR}/lib/" 2>/dev/null || true
cp "${QT_DESKTOP}/plugins/tls/libqopensslbackend.so" "${BUNDLE_DIR}/plugins/tls/"

# Wrapper — resolves libs relative to itself, inside the jm_twin_3cell tree only.
cat > "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh" << WRAPPER
#!/bin/bash
# Invoked by systemd: run_drywelldt_jm_3cell.sh <deployment_name>
set -e
DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
DEPLOYMENT="\$1"
if [[ -z "\$DEPLOYMENT" ]]; then
    echo "Usage: \$(basename "\$0") <deployment_name>" >&2
    exit 1
fi
export LD_LIBRARY_PATH="\${DIR}/../lib:\${LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\${DIR}/../plugins"
exec "\${DIR}/OHTwin" \\
    --deployment "${EC2_DEPLOY_ROOT}/\${DEPLOYMENT}"
WRAPPER
chmod +x "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh"

# Own systemd template, own namespace — distinct from drywelldt@ (Bioretention)
# and drywelldt-jm@ (the original, uncorrected JM) so neither is ever touched.
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

# Viewer config — absolute same-origin URLs under the domain.
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
# Step 4 — Prepare EC2, stop only this fork's own service
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
# Stop ONLY this fork's own instance. Bioretention's drywelldt@ units and the
# original JM's drywelldt-jm@ units are untouched -- different template names.
sudo systemctl stop "${SVC}@${D}.service" 2>/dev/null || true
ENDSSH

# =============================================================================
# Step 5 — Push JM 3-cell binary, libs, wrapper, unit, resources
# =============================================================================
section "Pushing JM 3-cell runner and libraries..."
scp "${SCP_OPTS[@]}" "${RUNNER_BIN}"                              "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/run_drywelldt_jm_3cell.sh"    "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/lib/*                        "${EC2_USER}@${EC2_HOST}:${EC2_LIB}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/plugins/tls/libqopensslbackend.so \
                                                                  "${EC2_USER}@${EC2_HOST}:${EC2_PLUGINS}/tls/"

log "Pushing OHQ templates to ${EC2_RESOURCES} ..."
scp "${SCP_OPTS[@]}" "${LOCAL_PROJECT}/resources"/*.json \
                     "${LOCAL_PROJECT}/resources"/*.list \
                                                                  "${EC2_USER}@${EC2_HOST}:${EC2_RESOURCES}/"
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
    echo "[remote] --fresh: wiping outputs/state/snapshots"
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
                // Never display:none the Qt container — it is measured to size
                // the canvas, and a hidden element measures 0x0.
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
# ${D} — forward (forecast-only) viewer, 3-cell corrected model. Generated by deploy_jm_3cell.sh.
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

# Not passed by default for this review copy -- the live landing page should
# only be repointed by a deliberate later decision, not as a side effect of
# standing this review copy up.
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
# Step 9 — Start this fork's own service
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
echo "----- Bioretention services (must still be running, untouched) -----"
systemctl list-units "drywelldt@*" --no-pager --no-legend
echo
echo "----- Original JM forecast (must still be running, untouched) -----"
systemctl list-units "drywelldt-jm@*" --no-pager --no-legend
ENDSSH

# =============================================================================
# Done
# =============================================================================
section "JM 3-cell deployment complete"
echo
echo "  Viewer:  http://${DOMAIN}/${PAGE}/"
echo "  Data:    http://${DOMAIN}/${PAGE}/outputs/"
if [[ "${SET_LANDING}" == "1" ]]; then
    echo "  Root:    http://${DOMAIN}/  ->  /${PAGE}/"
fi
echo
echo "  Logs:"
echo "    ssh -i \"${PEM_FILE}\" ${EC2_USER}@${EC2_HOST}"
echo "    sudo journalctl -u ${SERVICE_PREFIX}@${DEPLOYMENT} -f"
echo
echo "  Redeploy keeping accumulated state:  ./deploy_jm_3cell.sh"
echo "  Redeploy from scratch:               ./deploy_jm_3cell.sh --fresh"
