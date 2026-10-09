#!/usr/bin/env bash
set -euo pipefail

dpkg -s xgc2-vrpn-router >/dev/null
command -v xgc2-vrpn-router >/dev/null
xgc2-vrpn-router --version
test -f /usr/share/xgc2-vrpn-router/router.json
test -f /lib/systemd/system/xgc2-vrpn-router.service -o -f /usr/lib/systemd/system/xgc2-vrpn-router.service
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
python3 - "${work}" <<'PYINPUT'
import json, os, pathlib, sys
root=pathlib.Path(sys.argv[1]); application=json.loads(pathlib.Path('/usr/share/xgc2-vrpn-router/router.json').read_text())
binding={'schema_version':1,'target_id':'installed-check','service':'xgc2.vrpn-router','api_version':'1','profile':'http.v1','endpoint':{'kind':'unix','address':str(root/'management.sock')},'runtime_grant':'runtime','authentication':'local_private','secret_handles':{},'storage_grants':[]}
p=root/'bootstrap-input.json';p.write_text(json.dumps({'schema_version':1,'binding':binding,'grants':{},'application':application}));p.chmod(0o600)
PYINPUT
xgc2-vrpn-router --check-config --bootstrap-input "${work}/bootstrap-input.json"
if xgc2-vrpn-router --check-config --config /usr/share/xgc2-vrpn-router/router.json >"${work}/retired.log" 2>&1; then
  echo "router still accepted the retired separate config input" >&2
  exit 1
fi
ldd /usr/bin/xgc2-vrpn-router | awk '/not found/ {missing=1} END {exit missing ? 1 : 0}'
