#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VRPN_PREFIX="${VRPN_OFFICIAL_PREFIX:-/opt/xgc2/vrpn-official}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/official-vrpn-e2e}"
ROUTER_BINARY="${ROUTER_BINARY:-${REPO_ROOT}/build/xgc2-vrpn-router}"
UPSTREAM_PORT="${UPSTREAM_PORT:-43883}"
ROUTER_PORT="${ROUTER_PORT:-43884}"
TRACKER_NAME="${TRACKER_NAME:-Tracker0}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-12}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vrpn-prefix)
      VRPN_PREFIX="$2"
      shift 2
      ;;
    --work-dir)
      WORK_DIR="$2"
      shift 2
      ;;
    --router-binary)
      ROUTER_BINARY="$2"
      shift 2
      ;;
    --upstream-port)
      UPSTREAM_PORT="$2"
      shift 2
      ;;
    --router-port)
      ROUTER_PORT="$2"
      shift 2
      ;;
    --tracker-name)
      TRACKER_NAME="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT_SECONDS="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ ! -x "${ROUTER_BINARY}" ]]; then
  echo "router binary is not executable: ${ROUTER_BINARY}" >&2
  exit 1
fi

mkdir -p "${WORK_DIR}"
# The original independent official peer gate consumes managed binaries.
# Products never fetch or compile third-party VRPN sources.
VRPN_SERVER="${VRPN_PREFIX}/bin/vrpn_server"
VRPN_PRINT_DEVICES="${VRPN_PREFIX}/bin/vrpn_print_devices"
test -x "${VRPN_SERVER}"
test -x "${VRPN_PRINT_DEVICES}"

RUN_DIR="${WORK_DIR}/run"
rm -rf "${RUN_DIR}"
mkdir -m 0700 -p "${RUN_DIR}"

cat > "${RUN_DIR}/vrpn.cfg" <<EOF
vrpn_Tracker_NULL ${TRACKER_NAME} 2 30.0
EOF

python3 - "${RUN_DIR}" "${UPSTREAM_PORT}" "${ROUTER_PORT}" "${TRACKER_NAME}" <<'PYINPUT'
import json, pathlib, sys
root=pathlib.Path(sys.argv[1]); up,down=map(int,sys.argv[2:4]); tracker=sys.argv[4]
application={'schema_version':1,'upstream_host':'127.0.0.1','upstream_port':up,'bind_address':'127.0.0.1','listen_port':down,'mainloop_rate_hz':500,'upstream_update_rate_hz':30,'forwarding_enabled':True,'mappings':[{'upstream':tracker,'downstream':tracker,'sensors':2}]}
binding={'schema_version':1,'target_id':'official-interop','service':'xgc2.vrpn-router','api_version':'1','profile':'http.v1','endpoint':{'kind':'unix','address':str(root.resolve()/'management.sock')},'runtime_grant':'runtime','authentication':'local_private','secret_handles':{},'storage_grants':[]}
p=root/'bootstrap-input.json';p.write_text(json.dumps({'schema_version':1,'binding':binding,'grants':{},'application':application}));p.chmod(0o600)
PYINPUT

pids=()
cleanup() {
  local pid
  for pid in "${pids[@]:-}"; do
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  done
  wait "${pids[@]:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"${VRPN_SERVER}" -quiet -f "${RUN_DIR}/vrpn.cfg" "${UPSTREAM_PORT}" \
  >"${RUN_DIR}/server.log" 2>&1 &
pids+=("$!")
sleep 1

"${ROUTER_BINARY}" --bootstrap-input "${RUN_DIR}/bootstrap-input.json" \
  >"${RUN_DIR}/router.log" 2>&1 &
pids+=("$!")
sleep 1

client_status=0
timeout -s INT "${TIMEOUT_SECONDS}" stdbuf -oL -eL "${VRPN_PRINT_DEVICES}" \
  -nobutton -noanalog -nodial -notext -trackerstride 1 \
  "${TRACKER_NAME}@127.0.0.1:${ROUTER_PORT}" \
  >"${RUN_DIR}/client.log" 2>&1 || client_status=$?

if [[ "${client_status}" != "0" && "${client_status}" != "124" ]]; then
  echo "official vrpn_print_devices exited with status ${client_status}" >&2
  cat "${RUN_DIR}/client.log" >&2 || true
  exit 1
fi

if ! grep -Eq "Tracker ${TRACKER_NAME}@127\\.0\\.0\\.1:${ROUTER_PORT}, sensor [0-9]+:" "${RUN_DIR}/client.log"; then
  echo "official vrpn_print_devices did not receive a routed tracker report" >&2
  echo "---- server.log ----" >&2
  cat "${RUN_DIR}/server.log" >&2 || true
  echo "---- router.log ----" >&2
  cat "${RUN_DIR}/router.log" >&2 || true
  echo "---- client.log ----" >&2
  cat "${RUN_DIR}/client.log" >&2 || true
  exit 1
fi

echo "official VRPN E2E passed: ${TRACKER_NAME}@127.0.0.1:${UPSTREAM_PORT} -> xgc2-vrpn-router -> ${TRACKER_NAME}@127.0.0.1:${ROUTER_PORT}"
