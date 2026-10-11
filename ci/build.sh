#!/usr/bin/env bash
# Builds AutoBleem's OpenBIOS (openbios/ with the AutoBleem boot screen in shell/) and checks it:
#
#   ci/build.sh            dist/openbios-<version>.bin + its .sha256
#
# Runs in PCSX-Redux's own build image, which carries the MIPS toolchain (mipsel-none-elf):
#   docker run --rm -u $(id -u):$(id -g) -v $PWD:/project -w /project \
#       ghcr.io/grumpycoders/pcsx-redux-build:latest ci/build.sh
# The version is VERSION, or AB_VERSION (CI: the tag, or VERSION-<date>-<sha> for a nightly).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${AB_VERSION:-$(tr -d '\r\n' < VERSION)}"
JOBS="${JOBS:-$(nproc)}"

make -C shell clean >/dev/null
make -C openbios clean >/dev/null
make -C openbios -j "$JOBS"

bin=openbios/openbios.bin
size=$(stat -c %s "$bin")
[ "$size" -eq 524288 ] || { echo "openbios.bin is $size bytes, not 512 KiB" >&2; exit 1; }
# the shell carries the boot screen: its marker string must be in the ROM
grep -q "OpenBIOS tiny shell (AutoBleem)" "$bin" || { echo "openbios.bin lacks the AutoBleem shell" >&2; exit 1; }

mkdir -p dist
cp "$bin" "dist/openbios-$VERSION.bin"
(cd dist && sha256sum "openbios-$VERSION.bin" > "openbios-$VERSION.bin.sha256")
ls -l dist/
