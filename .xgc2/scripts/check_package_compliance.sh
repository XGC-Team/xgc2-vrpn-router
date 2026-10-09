#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

required_files=(
  ".xgc2/product.yml"
  ".xgc2/scripts/build_deb_in_docker.sh"
  ".xgc2/scripts/package_deb.sh"
  ".xgc2/scripts/check_installed_package.sh"
  ".xgc2/scripts/run_official_vrpn_e2e.sh"
  ".github/workflows/ci.yml"
  ".github/workflows/release.yml"
  "CMakeLists.txt"
  "config/router.json"
  "systemd/xgc2-vrpn-router.service"
  "src/main.cpp"
)

cd "${REPO_ROOT}"
for file in "${required_files[@]}"; do
  test -f "${file}" || {
    echo "missing required file: ${file}" >&2
    exit 1
  }
done

for script in .xgc2/scripts/*.sh; do
  test -x "${script}" || {
    echo "script is not executable: ${script}" >&2
    exit 1
  }
  bash -n "${script}"
done

grep -q '^id:[[:space:]]*xgc2-vrpn-router$' .xgc2/product.yml
product_version="$(awk -F': *' '/^version:/ {print $2; exit}' .xgc2/product.yml)"
[[ "${product_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$ ]]
grep -q '/usr/share/xgc2-vrpn-router/router.json' .xgc2/product.yml
grep -q 'ExecStart=/usr/bin/xgc2-vrpn-router --bootstrap-input /etc/xgc2/vrpn-router/bootstrap.json' systemd/xgc2-vrpn-router.service
python3 -c 'import json; d=json.load(open("config/router.json")); assert d["schema_version"] == 1; assert isinstance(d["mappings"], list)'

git diff --check
