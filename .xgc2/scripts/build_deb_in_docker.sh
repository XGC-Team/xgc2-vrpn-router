#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

UBUNTU_VERSION="${UBUNTU_VERSION:-20.04}"
DOCKER_IMAGE="${DOCKER_IMAGE:-}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/docker-${UBUNTU_VERSION}}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/debs}"
INSTALL_CHECK="${INSTALL_CHECK:-true}"
E2E_CHECK="${E2E_CHECK:-true}"
VRPN_NATIVE_PREFIX="${VRPN_NATIVE_PREFIX:-/opt/xgc2/vrpn-native}"
VRPN_OFFICIAL_PREFIX="${VRPN_OFFICIAL_PREFIX:-/opt/xgc2/vrpn-official}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ubuntu-version)
      UBUNTU_VERSION="$2"
      shift 2
      ;;
    --image)
      DOCKER_IMAGE="$2"
      shift 2
      ;;
    --work-dir)
      WORK_DIR="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --skip-install-check)
      INSTALL_CHECK=false
      shift
      ;;
    --skip-e2e)
      E2E_CHECK=false
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

case "${UBUNTU_VERSION}" in
  20.04) PACKAGE_DISTRIBUTION="focal" ;;
  22.04) PACKAGE_DISTRIBUTION="jammy" ;;
  24.04) PACKAGE_DISTRIBUTION="noble" ;;
  *)
    echo "unsupported Ubuntu version: ${UBUNTU_VERSION}" >&2
    exit 1
    ;;
esac
if [[ -z "${DOCKER_IMAGE}" ]]; then
  case "${PACKAGE_DISTRIBUTION}" in
    focal) DOCKER_IMAGE="ghcr.io/xgc-team/xgc2-images/xgc2-build-focal-full-noetic:1.0.10@sha256:20b1a11b74fd9114d95f75e43508909d9206d043c64669aeaa42214fd99d8f45" ;;
    jammy) DOCKER_IMAGE="ghcr.io/xgc-team/xgc2-images/xgc2-build-jammy-full-humble:1.0.5@sha256:3e5675964b95391b46e1c2eb7a5670b6cdc6c8eeda101558bffdb07fb6149d9a" ;;
    noble) DOCKER_IMAGE="ghcr.io/xgc-team/xgc2-images/xgc2-build-noble-full-jazzy:1.0.5@sha256:55a604b4642a3f966a3b30ee08331c81d4f5d08867cfb290974cdba368bf82bd" ;;
  esac
fi

mkdir -p "${WORK_DIR}" "${OUTPUT_DIR}"

docker pull "${DOCKER_IMAGE}"
docker run --rm \
  -e XGC2_APT_OVERLAY_URL="${XGC2_APT_OVERLAY_URL:-}" \
  -e DEBIAN_FRONTEND=noninteractive \
  -e INSTALL_CHECK="${INSTALL_CHECK}" \
  -e E2E_CHECK="${E2E_CHECK}" \
  -e PACKAGE_DISTRIBUTION="${PACKAGE_DISTRIBUTION}" \
  -e VRPN_NATIVE_PREFIX="${VRPN_NATIVE_PREFIX}" \
  -e VRPN_OFFICIAL_PREFIX="${VRPN_OFFICIAL_PREFIX}" \
  -v "${REPO_ROOT}:/workspace/vrpn-router:ro" \
  -v "${WORK_DIR}:/workspace/work" \
  -v "${OUTPUT_DIR}:/workspace/out" \
  "${DOCKER_IMAGE}" \
  bash -lc '
    set -euo pipefail

    export DEBIAN_FRONTEND=noninteractive
    for pkg in \
      build-essential ca-certificates cmake dpkg-dev fakeroot file git \
      ninja-build pkg-config rsync curl libjsoncpp-dev libusb-1.0-0-dev
    do
      if ! dpkg -s "${pkg}" >/dev/null 2>&1; then
        echo "image is missing ${pkg}; use xgc2-build-*-dev" >&2
        exit 1
      fi
    done

    if [[ "${PACKAGE_DISTRIBUTION}" == focal ]]; then export CC=clang-10 CXX=clang++-10; fi
    # Third-party native and pristine official wire peers are managed image inputs.
    test -f "${VRPN_NATIVE_PREFIX}/xgc2-vrpn-router-native.json"
    test -f "${VRPN_NATIVE_PREFIX}/lib/libvrpnserver.a"
    printf "%s  %s\n" "ea2bc9f761ce42c9b7d079f106059857557f037077cfc74b087aa945c80ab521" \
      "${VRPN_NATIVE_PREFIX}/xgc2-vrpn-router-native.json" | sha256sum -c -
    test -x "${VRPN_OFFICIAL_PREFIX}/bin/vrpn_server"
    test -x "${VRPN_OFFICIAL_PREFIX}/bin/vrpn_print_devices"
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL --retry 5 https://xgc2.apt.xiaokang.ink/xgc2-archive-keyring.gpg \
      -o /etc/apt/keyrings/xgc2-archive-keyring.gpg
    chmod 0644 /etc/apt/keyrings/xgc2-archive-keyring.gpg
    source_url="${XGC2_APT_OVERLAY_URL:-https://xgc2.apt.xiaokang.ink}"
    printf "deb [signed-by=/etc/apt/keyrings/xgc2-archive-keyring.gpg] %s %s main\n" \
      "${source_url%/}" "${PACKAGE_DISTRIBUTION}" > /etc/apt/sources.list.d/xgc2.list
    apt-get update -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/xgc2.list -o Dir::Etc::sourceparts="-" -o APT::Get::List-Cleanup="0"
    distribution="${PACKAGE_DISTRIBUTION}"
    apt-get install -y --no-install-recommends "libxgc2-xrpc-dev=0.1.0-1~${distribution}"
    rm -rf /workspace/work/src /workspace/work/build /workspace/work/install-root

    mkdir -p /workspace/work/src
    rsync -a --delete /workspace/vrpn-router/ /workspace/work/src/

    cd /workspace/work/src
    cmake -S . -B /workspace/work/build \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DVRPN_ROOT="${VRPN_NATIVE_PREFIX}" \
      -DCMAKE_PREFIX_PATH="${VRPN_NATIVE_PREFIX};/usr" \
      -DVRPN_INTEROP_ROOT="${VRPN_OFFICIAL_PREFIX}"
    cmake --build /workspace/work/build
    (cd /workspace/work/build && ctest --output-on-failure)
    DESTDIR=/workspace/work/install-root cmake --install /workspace/work/build

    /workspace/vrpn-router/.xgc2/scripts/package_deb.sh \
      --install-root /workspace/work/install-root \
      --output-dir /workspace/out \
      --distro "${PACKAGE_DISTRIBUTION}"

    if [[ "${INSTALL_CHECK}" == "true" ]]; then
      apt-get install -y --no-install-recommends /workspace/out/xgc2-vrpn-router_*.deb
      /workspace/vrpn-router/.xgc2/scripts/check_installed_package.sh
    fi

    if [[ "${E2E_CHECK}" == "true" ]]; then
      /workspace/vrpn-router/.xgc2/scripts/run_official_vrpn_e2e.sh \
        --vrpn-prefix "${VRPN_OFFICIAL_PREFIX}" \
        --work-dir /workspace/work/e2e \
        --router-binary /usr/bin/xgc2-vrpn-router
    fi
  '

echo "Debian package output:"
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name "*.deb" -print | sort
