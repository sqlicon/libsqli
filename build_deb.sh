#!/usr/bin/env bash
# Build a .deb package for sqlicon
# Usage: ./build_deb.sh [output_dir]
set -euo pipefail

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${1:-${SRCDIR}/../dist}"
VERSION="$(grep -oP 'project\(\s*\S+\s+VERSION\s+\K[0-9.]+' "${SRCDIR}/CMakeLists.txt")"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
PKG="sqlicon_${VERSION}_${ARCH}"
STAGING="${OUTDIR}/${PKG}"

mkdir -p "${OUTDIR}"

# Build release binary
echo "==> Building sqlicon..."
BUILDDIR="${SRCDIR}/build-deb"
mkdir -p "${BUILDDIR}"
cd "${BUILDDIR}"
cmake "${SRCDIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSQLI_ENABLE_SANITIZERS=OFF \
    -DSQLI_ENABLE_LIVE_TESTS=OFF
make sqlicon -j"$(nproc)"

# Create package staging
echo "==> Staging package..."
rm -rf "${STAGING}"
mkdir -p "${STAGING}/usr/bin"
mkdir -p "${STAGING}/usr/share/man/man1"
mkdir -p "${STAGING}/DEBIAN"

cp "${BUILDDIR}/sqlicon" "${STAGING}/usr/bin/sqlicon"
cp "${SRCDIR}/tools/sqlicon.1" "${STAGING}/usr/share/man/man1/sqlicon.1"
gzip -f "${STAGING}/usr/share/man/man1/sqlicon.1"

# Write control file
cat > "${STAGING}/DEBIAN/control" <<CTRL
Package: sqlicon
Version: ${VERSION}
Section: database
Priority: optional
Architecture: ${ARCH}
Depends: libssl3t64 | libssl3 | libssl-dev
Maintainer: the libisql authors <sqlicon0x7bc@gmail.com>
Description: Interactive shell for Informix databases
 Sqlicon is a command-line interface for IBM Informix databases that
 connects directly using the Informix SQLI wire protocol. No Informix
 Client SDK is required.
 .
 Features include:
  * Interactive SQL prompt with command history
  * Batch mode for scripting and automation
  * Multiple output formats: aligned, CSV, JSON, line, markdown
  * Encrypted connection profiles
  * Schema introspection (tables, indexes, constraints)
  * Data export (INSERT dump) and CSV import
  * TLS-encrypted connections via OpenSSL
CTRL

# Build the package
DEB="${OUTDIR}/sqlicon_${VERSION}_${ARCH}.deb"
echo "==> Building ${DEB}..."
dpkg-deb --build --root-owner-group "${STAGING}" "${DEB}"

# Clean up staging
rm -rf "${STAGING}"

echo "==> Done: ${DEB}"
dpkg-deb --info "${DEB}"
echo ""
echo "Contents:"
dpkg-deb --contents "${DEB}"
