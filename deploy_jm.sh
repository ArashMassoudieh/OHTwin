#!/bin/bash
# =============================================================================
# JM (John McCormack Rd) Bioretention — AWS Deploy Script
#
# Usage:
#   ./deploy_jm.sh [--fresh] [--set-landing]
#
#   --fresh        Wipe outputs/, state/ and snapshots/ on EC2 before starting.
#                  Omit it to redeploy in place and let the runner continue
#                  from the state it already has.
#   --set-landing  Point openhydrotwin.com/ at this page instead of
#                  /Bioretention_assimilation/. One-time; harmless to repeat.
#
# What this deploys
#   A single forward (forecast-only) deployment: JM_forecast. The runner pulls
#   Open-Meteo weather for the site, steps the model once a day, and writes a
#   7-day forecast. There is no truth and no assimilation for JM yet.
#
# Isolation from Bioretention  (this is the point of a separate script)
#   Everything JM touches is suffixed or namespaced so the two live
#   Bioretention services keep running the binary they were deployed with:
#     everything    /home/ubuntu/jm_twin/         (a tree of its own)
#       binary        jm_twin/app/bin/OHTwin
#       libs          jm_twin/app/lib/
#       templates     jm_twin/resources/          (see EC2 layout note below)
#       deployments   jm_twin/deployments/
#     systemd unit  drywelldt-jm@.service         (NOT drywelldt@)
#     nginx         /etc/nginx/ohtwin-locations/JM_forecast.conf
#   Nothing under /home/ubuntu/drywelldt/ or /home/ubuntu/resources/ is
#   written by this script, so Bioretention keeps its binary AND its templates.
#
# Addressing (verified against the live server)
#   page   http://openhydrotwin.com/JM_Bioretention_forecast/
#   data   http://openhydrotwin.com/JM_Bioretention_forecast/outputs/...
#   Both are path-based on port 80, matching the routing already used for the
#   Bioretention pages. No new firewall port is required. Note that
#   deployment.port in config.json is metadata only — DTConfig validates it is
#   positive but nothing binds it (DTConfig.cpp:169).
#
# nginx integration
#   The openhydrotwin server block is shared with Bioretention, so this script
#   does not rewrite it. It adds one `include` line (once, with a backup) and
#   thereafter only writes its own file in /etc/nginx/ohtwin-locations/.
#   Every nginx change is validated with `nginx -t` and rolled back on failure.
# =============================================================================

set -e

# --- Configuration ---------------------------------------------------------
EC2_USER="ubuntu"
# Elastic IP (openhydrotwin.com). Stable across instance stop/start.
EC2_PUBLIC_IP="52.42.223.42"
EC2_HOST="${EC2_PUBLIC_IP}"
DOMAIN="openhydrotwin.com"

# NOTE: deploy.sh still points at the old Dropbox "(Selective Sync Conflict)"
# folder, which no longer exists. The key now lives here.
PEM_FILE="${PEM_FILE:-/home/arash/Dropbox/AWS_/ArashLinux.pem}"

LOCAL_PROJECT="/home/arash/Projects/DrywellDT"
LOCAL_VIEWER="${LOCAL_PROJECT}/viewer"

DEPLOYMENT="JM_forecast"          # directory under deployments/
PAGE="JM_Bioretention_forecast"   # URL path and /var/www subdirectory

# Qt install. deploy.sh pins 6.8.2, which is not present on this machine;
# 6.8.3 is. The runner only needs QT += core network, so any 6.8.x works.
QT_VER="${QT_VER:-6.8.3}"
QT_ROOT="/home/arash/Qt/${QT_VER}"
QT_DESKTOP="${QT_ROOT}/gcc_64"
QMAKE_DESKTOP="${QT_DESKTOP}/bin/qmake"
QT_LIB_DIR="${QT_DESKTOP}/lib"
OHQ_LIB_DIR="${LOCAL_PROJECT}/libs/release"

# WebAssembly viewer. emsdk is not installed on this machine, so we reuse the
# already-built viewer by default. Set SKIP_VIEWER_BUILD=0 (and install emsdk
# 3.1.56 plus Qt wasm_singlethread) to rebuild it.
EMSDK_ENV="/home/arash/emsdk/emsdk_env.sh"
SKIP_VIEWER_BUILD="${SKIP_VIEWER_BUILD:-1}"
VIEWER_BUILD="${LOCAL_VIEWER}/build/WebAssembly_Qt_6_8_2_single_threaded-Release"

PRO_FILE="${LOCAL_PROJECT}/OHTwin.pro"
RUNNER_BINARY_NAME="OHTwin"
# OHTwin.pro sets BUILD_DIR = $$PWD/build-qmake, and $$PWD is the directory
# holding the .pro — so DESTDIR/OBJECTS_DIR/MOC_DIR are fixed absolute paths
# regardless of where qmake is invoked. A shadow build directory would only
# ever receive the Makefile, so build where deploy.sh builds.
RUNNER_BUILD="${LOCAL_PROJECT}/build-qmake"
RUNNER_BIN="${RUNNER_BUILD}/bin/${RUNNER_BINARY_NAME}"

# EC2 layout — an entirely separate tree from /home/ubuntu/drywelldt.
#
# The nesting depth here is load-bearing. DTRunner.cpp:855 resolves the OHQ
# template directory as applicationDirPath() + "/../../resources/", so the
# grandparent of the binary's directory decides which templates get loaded:
#
#   /home/ubuntu/drywelldt/bin/OHTwin      -> /home/ubuntu/resources        (Bioretention)
#   /home/ubuntu/jm_twin/app/bin/OHTwin    -> /home/ubuntu/jm_twin/resources (JM)
#
# Putting the binary one level deeper is what gives JM its own template set.
# The earlier bin_jm/ layout resolved to the shared /home/ubuntu/resources,
# where Sewer_system.json predates the Street Gutter Segment type — which is
# why the four gutters silently failed to instantiate on the server while
# working locally.
EC2_ROOT="/home/ubuntu/jm_twin"
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
        -h|--help)     sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Convenience -----------------------------------------------------------
SSH_OPTS=(-i "${PEM_FILE}" "${EC2_USER}@${EC2_HOST}")
SCP_OPTS=(-i "${PEM_FILE}")

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()    { echo -e "${GREEN}[jm]${NC} $1"; }
warn()   { echo -e "${YELLOW}[jm]${NC} $1"; }
err()    { echo -e "${RED}[jm]${NC} $1"; exit 1; }
section(){ echo -e "${GREEN}[jm]${NC} =================================================="; \
           echo -e "${GREEN}[jm]${NC} $1"; \
           echo -e "${GREEN}[jm]${NC} =================================================="; }

# =============================================================================
# Step 0 — Pre-flight
# =============================================================================
section "JM deploy — ${DEPLOYMENT} -> /${PAGE}/  (fresh=${FRESH})"

[[ -f "${PEM_FILE}" ]] || err "SSH key not found: ${PEM_FILE}"
[[ -d "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}" ]] || \
    err "Local deployment not found: deployments/${DEPLOYMENT}"
[[ -f "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" ]] || \
    err "No config.json in deployments/${DEPLOYMENT}"
[[ -x "${QMAKE_DESKTOP}" ]] || \
    err "qmake not found at ${QMAKE_DESKTOP}. Set QT_VER to an installed Qt (have: $(ls -d /home/arash/Qt/6.* 2>/dev/null | xargs -n1 basename | paste -sd' '))."

# Refuse to run if the deployment still carries assimilation config or noise.
if python3 - "${LOCAL_PROJECT}/deployments/${DEPLOYMENT}/config.json" << 'PYCHK'
import json, sys
d = json.load(open(sys.argv[1]))
bad = []
if "assimilation" in d:
    bad.append("has an 'assimilation' section (this is a forecast-only deployment)")
ns = d.get("observations", {}).get("noise_sigma", {})
nz = {k: v for k, v in ns.items() if v}
if nz:
    bad.append("has non-zero noise_sigma: %s" % ", ".join(sorted(nz)))
if d["deployment"]["name"] != "JM_forecast":
    bad.append("deployment.name is %r, expected 'JM_forecast'" % d["deployment"]["name"])
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
# `make` exits nonzero on a real failure and set -e aborts us, so reaching the
# next line means the binary is up to date with its sources. "Nothing to be
# done" is a successful incremental build, not a problem.
make -j"$(nproc)"
[[ -x "${RUNNER_BIN}" ]] || err "Build produced no binary at ${RUNNER_BIN}"

# Belt and braces: warn if any tracked source is newer than the binary, which
# would mean make's dependency graph missed something.
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
section "Staging JM artifacts..."
BUNDLE_DIR="/tmp/drywelldt_jm_bundle"
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

# Wrapper — resolves libs relative to itself, inside the JM tree only.
cat > "${BUNDLE_DIR}/run_drywelldt_jm.sh" << WRAPPER
#!/bin/bash
# Invoked by systemd: run_drywelldt_jm.sh <deployment_name>
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
chmod +x "${BUNDLE_DIR}/run_drywelldt_jm.sh"

# Separate systemd template so `systemctl restart drywelldt@...` never hits JM
# and vice versa.
cat > "${BUNDLE_DIR}/drywelldt-jm@.service" << 'UNIT'
[Unit]
Description=DrywellDT Digital Twin - JM (%i)
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/jm_twin/app/bin
ExecStart=/home/ubuntu/jm_twin/app/bin/run_drywelldt_jm.sh %i
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=drywelldt-jm-%i

[Install]
WantedBy=multi-user.target
UNIT

# Viewer config — absolute same-origin URLs under the domain. Same-origin means
# no CORS preflight, and no dependence on the Elastic IP.
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
# Step 4 — Prepare EC2, stop only the JM service
# =============================================================================
section "Preparing EC2 (JM paths only)..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" << 'ENDSSH'
set -e
D="$1"
mkdir -p /home/ubuntu/jm_twin/app/{bin,lib,plugins/tls}
mkdir -p /home/ubuntu/jm_twin/{resources,deployments}
sudo mkdir -p /var/www/drywelldt
sudo chown -R ubuntu:ubuntu /var/www/drywelldt
sudo mkdir -p /etc/nginx/ohtwin-locations
# Stop ONLY the JM instance. Bioretention's drywelldt@ units are untouched.
sudo systemctl stop "drywelldt-jm@${D}.service" 2>/dev/null || true

# One-time migration off the previous bin_jm/ layout, whose binary resolved
# templates to the SHARED /home/ubuntu/resources. Only ever removes JM-specific
# paths; /home/ubuntu/drywelldt/{bin,lib,resources} and the Bioretention
# deployments are never touched.
for stale in /home/ubuntu/drywelldt/bin_jm \
             /home/ubuntu/drywelldt/lib_jm \
             /home/ubuntu/drywelldt/plugins_jm \
             "/home/ubuntu/drywelldt/deployments/${D}"; do
    if [[ -e "$stale" ]]; then
        echo "[remote] removing stale JM path: $stale"
        rm -rf "$stale"
    fi
done
ENDSSH

# =============================================================================
# Step 5 — Push JM binary, libs, wrapper, unit, resources
# =============================================================================
section "Pushing JM runner and libraries..."
scp "${SCP_OPTS[@]}" "${RUNNER_BIN}"                        "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/run_drywelldt_jm.sh"    "${EC2_USER}@${EC2_HOST}:${EC2_BIN}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/lib/*                  "${EC2_USER}@${EC2_HOST}:${EC2_LIB}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}"/plugins/tls/libqopensslbackend.so \
                                                            "${EC2_USER}@${EC2_HOST}:${EC2_PLUGINS}/tls/"

# OHQ templates. This must land in ${EC2_RESOURCES} — the directory the
# ../../resources rule resolves to from ${EC2_BIN} — or the runner silently
# falls back to whatever templates live there and loads the wrong types.
log "Pushing OHQ templates to ${EC2_RESOURCES} ..."
scp "${SCP_OPTS[@]}" "${LOCAL_PROJECT}/resources"/*.json \
                     "${LOCAL_PROJECT}/resources"/*.list \
                                                            "${EC2_USER}@${EC2_HOST}:${EC2_RESOURCES}/"
scp "${SCP_OPTS[@]}" "${BUNDLE_DIR}/drywelldt-jm@.service"  "${EC2_USER}@${EC2_HOST}:/tmp/drywelldt-jm@.service"
ssh "${SSH_OPTS[@]}" bash -s << 'ENDSSH'
set -e
sudo mv /tmp/drywelldt-jm@.service /etc/systemd/system/drywelldt-jm@.service
sudo systemctl daemon-reload
chmod +x /home/ubuntu/jm_twin/app/bin/OHTwin
chmod +x /home/ubuntu/jm_twin/app/bin/run_drywelldt_jm.sh
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
BASE="/home/ubuntu/jm_twin/deployments/${D}"
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

# Replace the stock Qt logo loading screen with the project splash, and drive
# it from the Qt loader's lifecycle. Operates on the staged copy, so a viewer
# rebuild never has to be re-patched.
SPLASH_FRAGMENT="${LOCAL_VIEWER}/splash_loading.html"
if [[ -f "${SPLASH_FRAGMENT}" ]]; then
    log "  Injecting loading splash..."
    python3 - "${VIEW_STAGE}/OHTwinViewer.html" "${SPLASH_FRAGMENT}" << 'PYEOF'
import re, sys
page_path, frag_path = sys.argv[1], sys.argv[2]
page = open(page_path).read()
frag = open(frag_path).read()

# 1. Swap the <figure id="qtspinner"> ... </figure> block for the splash.
m = re.search(r'<figure[^>]*id="qtspinner".*?</figure>', page, re.S)
if not m:
    sys.exit("could not find the #qtspinner block in %s" % page_path)
page = page[:m.start()] + frag + page[m.end():]

# 2. The stock page writes plain text into #qtstatus, which the splash does not
#    have. Route those writes to the splash instead of throwing on null.
page = page.replace(
    "const status = document.querySelector('#qtstatus');",
    "const status = document.querySelector('#qtstatus') || { set innerHTML(v) {\n"
    "                var s = document.getElementById('dt-status');\n"
    "                if (s) s.textContent = String(v).replace(/<[^>]*>/g, '');\n"
    "            }, get innerHTML() { return ''; } };")

# 3. Keep the Qt container laid out for the whole load.
#    Qt's stock showUi() sets #screen to display:none while the loading screen
#    is up. The WASM plugin measures that container to size its canvas, and a
#    display:none element measures 0x0 — so Qt builds a zero-size canvas and
#    paints nothing. Revealing it afterwards does not trigger a re-measure,
#    which is why the page stayed blank until opening devtools resized the
#    window and forced one. Overlay the splash instead of hiding the canvas.
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

# The splash paints its own full-viewport background, so it only needs to float.
page = page.replace(
    "#screen { width: 100%; height: 100%; }",
    "#screen { width: 100%; height: 100%; }\n"
    "      #qtspinner { position: fixed; inset: 0; z-index: 9999; margin: 0; }", 1)

# 4. Fade the splash out once Qt has loaded, and bring it back on exit.
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

# Our own location file — the only file this script rewrites on every run.
sudo tee "${LOCDIR}/${D}.conf" > /dev/null << EOF
# ${D} — forward (forecast-only) viewer. Generated by deploy_jm.sh.
# Data first: more specific paths must precede the viewer location.
location /${PAGE}/outputs/ {
    alias /home/ubuntu/jm_twin/deployments/${D}/outputs/;
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

# One-time: add the include line to the shared server block.
if ! grep -q "ohtwin-locations" "${SITE}"; then
    echo "[remote] adding include line to ${SITE} (backup: ${BACKUP})"
    sudo cp "${SITE}" "${BACKUP}"
    # Insert immediately before the final closing brace of the server block.
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

# Optional: repoint the site root at this page.
if [[ "${SET_LANDING}" == "1" ]]; then
    if ! grep -q "return 302 /${PAGE}/;" "${SITE}"; then
        echo "[remote] repointing site root to /${PAGE}/"
        [[ -f "${BACKUP}" ]] || sudo cp "${SITE}" "${BACKUP}"
        sudo sed -i "s|return 302 /Bioretention_assimilation/;|return 302 /${PAGE}/;|" "${SITE}"
        NEED_RELOAD=1
    fi
fi

# Validate. If nginx rejects the config, restore and abort so the live site
# is never left broken.
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
# Step 9 — Start the JM service
# =============================================================================
section "Starting drywelldt-jm@${DEPLOYMENT} ..."
ssh "${SSH_OPTS[@]}" bash -s -- "${DEPLOYMENT}" << 'ENDSSH'
set -e
D="$1"
sudo systemctl enable "drywelldt-jm@${D}.service"
sudo systemctl restart "drywelldt-jm@${D}.service"
sleep 5
sudo systemctl status "drywelldt-jm@${D}.service" --no-pager | head -18
echo
echo "----- Bioretention services (must still be running) -----"
systemctl list-units "drywelldt@*" --no-pager --no-legend
ENDSSH

# =============================================================================
# Done
# =============================================================================
section "JM deployment complete"
echo
echo "  Viewer:  http://${DOMAIN}/${PAGE}/"
echo "  Data:    http://${DOMAIN}/${PAGE}/outputs/"
if [[ "${SET_LANDING}" == "1" ]]; then
    echo "  Root:    http://${DOMAIN}/  ->  /${PAGE}/"
fi
echo
echo "  Logs:"
echo "    ssh -i \"${PEM_FILE}\" ${EC2_USER}@${EC2_HOST}"
echo "    sudo journalctl -u drywelldt-jm@${DEPLOYMENT} -f"
echo
echo "  Redeploy keeping accumulated state:  ./deploy_jm.sh"
echo "  Redeploy from scratch:               ./deploy_jm.sh --fresh"
