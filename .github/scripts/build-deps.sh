#!/usr/bin/env bash
# Build the libretro core's out-of-apt dependencies from source into a prefix.
#
# Ubuntu has no SDL3 / rapidyaml(ryml) / plutovg / plutosvg packages, and we build
# lz4 from source too so the versions match the validated recalbox recipe. Everything
# else the core needs (png, jpeg, zlib, zstd, webp, freetype, fontconfig, curl, pcap,
# dbus, udev) comes from apt — see the workflow.
#
# Usage: build-deps.sh <install-prefix>
set -euo pipefail

PREFIX="${1:?usage: build-deps.sh <install-prefix>}"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

# Pinned versions — match the validated recalbox build.
LZ4_VER=1.9.4
SDL3_VER=3.2.8
RYML_VER=0.12.1
PLUTOVG_VER=1.3.2
PLUTOSVG_VER=0.0.7

JOBS="$(nproc)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

CMAKE_COMMON=(
	-G Ninja
	-DCMAKE_BUILD_TYPE=Release
	-DBUILD_SHARED_LIBS=ON
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
	-DCMAKE_PREFIX_PATH="$PREFIX"
)

dl() { curl -fsSL --retry 3 -o "$2" "$1"; }

build() { # <src-dir> <build-dir> [extra cmake args...]
	local src="$1" bld="$2"; shift 2
	cmake -S "$src" -B "$bld" "${CMAKE_COMMON[@]}" "$@"
	cmake --build "$bld" -j "$JOBS"
	cmake --install "$bld"
}

echo "==> lz4 ${LZ4_VER}"
dl "https://github.com/lz4/lz4/archive/refs/tags/v${LZ4_VER}.tar.gz" lz4.tgz
tar xf lz4.tgz
build "lz4-${LZ4_VER}/build/cmake" build-lz4

echo "==> SDL3 ${SDL3_VER}"
dl "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VER}/SDL3-${SDL3_VER}.tar.gz" sdl3.tgz
tar xf sdl3.tgz
build "SDL3-${SDL3_VER}" build-sdl3 \
	-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF

echo "==> rapidyaml (ryml) ${RYML_VER}  [git --recursive for the c4core submodule]"
git clone --recursive --depth 1 --branch "v${RYML_VER}" \
	https://github.com/biojppm/rapidyaml.git ryml
build ryml build-ryml -DRYML_BUILD_TESTS=OFF -DRYML_BUILD_API=OFF

echo "==> plutovg ${PLUTOVG_VER}"
dl "https://github.com/sammycage/plutovg/archive/refs/tags/v${PLUTOVG_VER}.tar.gz" plutovg.tgz
tar xf plutovg.tgz
build "plutovg-${PLUTOVG_VER}" build-plutovg

echo "==> plutosvg ${PLUTOSVG_VER}  [needs plutovg]"
dl "https://github.com/sammycage/plutosvg/archive/refs/tags/v${PLUTOSVG_VER}.tar.gz" plutosvg.tgz
tar xf plutosvg.tgz
build "plutosvg-${PLUTOSVG_VER}" build-plutosvg -DPLUTOSVG_BUILD_EXAMPLES=OFF

echo "==> dependencies installed into: $PREFIX"
ls -la "$PREFIX/lib"
