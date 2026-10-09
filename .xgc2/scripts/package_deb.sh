#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INSTALL_ROOT=""
OUTPUT_DIR=""
DISTRO="${PACKAGE_DISTRO:-}"

product_version() {
  awk -F': *' '/^version:[[:space:]]*/ {print $2; exit}' "${REPO_ROOT}/.xgc2/product.yml"
}

VERSION="${PACKAGE_VERSION:-$(product_version)}"
if [[ -z "${VERSION}" ]]; then
  echo "package version is missing; set PACKAGE_VERSION or .xgc2/product.yml version" >&2
  exit 1
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-root)
      INSTALL_ROOT="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --distro)
      DISTRO="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${INSTALL_ROOT}" || -z "${OUTPUT_DIR}" ]]; then
  echo "--install-root and --output-dir are required" >&2
  exit 1
fi

if [[ -n "${DISTRO}" && "${VERSION}" != *"+${DISTRO}" ]]; then
  VERSION="${VERSION}+${DISTRO}"
fi

ARCH="$(dpkg --print-architecture)"
PACKAGE="xgc2-vrpn-router"
BUILD_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

mkdir -p "${OUTPUT_DIR}"
rm -f "${OUTPUT_DIR}/${PACKAGE}_"*.deb

pkg_root="${BUILD_DIR}/${PACKAGE}"
mkdir -p \
  "${pkg_root}/DEBIAN" \
  "${pkg_root}/usr/share/doc/${PACKAGE}"

for path in \
  /usr/bin/xgc2-vrpn-router \
  /usr/share/xgc2-vrpn-router/router.json \
  /lib/systemd/system/xgc2-vrpn-router.service \
  /usr/lib/systemd/system/xgc2-vrpn-router.service; do
  if [[ -e "${INSTALL_ROOT}${path}" ]]; then
    mkdir -p "${pkg_root}$(dirname "${path}")"
    cp -a "${INSTALL_ROOT}${path}" "${pkg_root}${path}"
  fi
done

if [[ ! -x "${pkg_root}/usr/bin/xgc2-vrpn-router" ]]; then
  echo "missing installed /usr/bin/xgc2-vrpn-router" >&2
  exit 1
fi

if [[ ! -f "${pkg_root}/usr/share/xgc2-vrpn-router/router.json" ]]; then
  echo "missing installed /usr/share/xgc2-vrpn-router/router.json" >&2
  exit 1
fi

if [[ ! -f "${pkg_root}/lib/systemd/system/xgc2-vrpn-router.service" &&
      ! -f "${pkg_root}/usr/lib/systemd/system/xgc2-vrpn-router.service" ]]; then
  echo "missing installed xgc2-vrpn-router.service" >&2
  exit 1
fi

find "${pkg_root}" -type d -exec chmod 0755 {} +
find "${pkg_root}" -type f -exec chmod 0644 {} +
chmod 0755 "${pkg_root}/usr/bin/xgc2-vrpn-router"
strip --strip-unneeded "${pkg_root}/usr/bin/xgc2-vrpn-router" 2>/dev/null || true

# All shared-library dependencies come from the actual installed executable.
# Missing image shlibs metadata fails the original package gate.
mkdir -p "${BUILD_DIR}/debian"
cat > "${BUILD_DIR}/debian/control" <<EOF
Source: ${PACKAGE}
Section: net
Priority: optional
Maintainer: XGC2 <apt@example.com>
Standards-Version: 4.6.0

Package: ${PACKAGE}
Architecture: any
Description: XGC2 VRPN router
 Native XRPC management and VRPN tracker relay.
EOF
shlibs_depends="$(cd "${BUILD_DIR}" && dpkg-shlibdeps -O "${pkg_root}/usr/bin/xgc2-vrpn-router" | sed -n "s/^shlibs:Depends=//p")"
[[ -n "${shlibs_depends}" ]] || { echo "missing actual shared-library dependencies" >&2; exit 1; }
# The service unit is optional; existing maintainer scripts guard systemctl.
depends="${shlibs_depends}, netbase"

cat > "${pkg_root}/DEBIAN/control" <<EOF
Package: ${PACKAGE}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: XGC2 <apt@example.com>
Depends: ${depends}
Description: XGC2 VRPN router
 Protocol-level VRPN tracker relay. It connects to one upstream VRPN server
 as a client and exposes configured trackers through a local VRPN server.
EOF

cat > "${pkg_root}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

if command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload >/dev/null 2>&1 || true
fi

exit 0
EOF

cat > "${pkg_root}/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ] || [ "$1" = "deconfigure" ]; then
  if command -v systemctl >/dev/null 2>&1; then
    systemctl stop xgc2-vrpn-router.service >/dev/null 2>&1 || true
  fi
fi

exit 0
EOF

cat > "${pkg_root}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e

if command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload >/dev/null 2>&1 || true
fi

if [ "$1" = "purge" ]; then
  rmdir /etc/xgc2/vrpn-router >/dev/null 2>&1 || true
  rmdir /etc/xgc2 >/dev/null 2>&1 || true
fi

exit 0
EOF

cat > "${pkg_root}/usr/share/doc/${PACKAGE}/README" <<EOF
XGC2 VRPN Router

Installed command:
  xgc2-vrpn-router

Default configuration:
  /usr/share/xgc2-vrpn-router/router.json (application domain example)

Systemd unit:
  xgc2-vrpn-router.service

The package installs the service unit but does not enable or start it by default.
The deployment owner supplies /etc/xgc2/vrpn-router/bootstrap.json, a private
BootstrapInput whose application is the router domain JSON. Then enable with:
  systemctl enable --now xgc2-vrpn-router.service
EOF

if [[ -n "${DISTRO}" ]]; then
  printf 'Built for Ubuntu distribution: %s\n' "${DISTRO}" > "${pkg_root}/usr/share/doc/${PACKAGE}/distribution"
fi

find "${pkg_root}/DEBIAN" "${pkg_root}/usr/share/doc/${PACKAGE}" -type f -exec chmod 0644 {} +
chmod 0755 "${pkg_root}/DEBIAN/postinst" "${pkg_root}/DEBIAN/prerm" "${pkg_root}/DEBIAN/postrm"
chmod 0755 "${pkg_root}/DEBIAN"
fakeroot dpkg-deb --build "${pkg_root}" "${OUTPUT_DIR}/${PACKAGE}_${VERSION}_${ARCH}.deb" >/dev/null
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name "${PACKAGE}_*.deb" -print | sort
