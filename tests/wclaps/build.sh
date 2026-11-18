#!/usr/bin/env bash
# Builds the test WCLAPs into dist/<name>.wclap.tar.gz (OUT=<dir> to build elsewhere, as CMake does)
# Needs Homebrew LLVM (clang with the wasm32 target) and lld (wasm-ld).
set -euo pipefail
cd "$(dirname "$0")"

CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
WASM_LD="${WASM_LD:-$(command -v wasm-ld || echo /opt/homebrew/opt/lld/bin/wasm-ld)}"
CLAP_INCLUDE="${CLAP_INCLUDE:-../../build/_deps/wclap-bridge-src/modules/clap/include}"
[ -d "$CLAP_INCLUDE" ] || { echo "CLAP headers not found at $CLAP_INCLUDE (configure the plugin first, or set CLAP_INCLUDE)" >&2; exit 1; }

OUT="${OUT:-.}"
BUILD="$OUT/build"
DIST="$OUT/dist"
rm -rf "$BUILD" "$DIST"
mkdir -p "$BUILD" "$DIST"

CFLAGS=(--target=wasm32 -ffreestanding -nostdlib -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
        -fno-builtin -I"$CLAP_INCLUDE" -Isrc)
LDFLAGS=(--no-entry --export=clap_entry --export=malloc --export=free --export-table --growable-table
         --stack-first -z stack-size=65536 --strip-debug)

"$CLANG" "${CFLAGS[@]}" -c src/rt.c -o "$BUILD/rt.o"

build() { # <name> <define> [ui dir]
  local name=$1 define=$2 ui=${3:-}
  local root="$BUILD/$name/$name.wclap"
  mkdir -p "$root"
  "$CLANG" "${CFLAGS[@]}" -D"$define" -c src/plugin.c -o "$BUILD/$name.o"
  "$WASM_LD" "${LDFLAGS[@]}" "$BUILD/$name.o" "$BUILD/rt.o" -o "$root/module.wasm"
  if [ -n "$ui" ]; then cp -R "$ui" "$root/ui"; fi
  COPYFILE_DISABLE=1 tar -czf "$DIST/$name.wclap.tar.gz" -C "$BUILD/$name" "$name.wclap"
  printf '%-22s %6s bytes  %s\n' "$name" "$(wc -c < "$root/module.wasm" | tr -d ' ')" "$DIST/$name.wclap.tar.gz"
}

build test-gain          TEST_GAIN          ui/gain
build test-lookahead     TEST_LOOKAHEAD
build test-sine          TEST_SINE
build test-broken-state  TEST_BROKEN_STATE
build test-param-events  TEST_PARAM_EVENTS  ui/gain
build test-web-features  TEST_WEB_FEATURES  ui/features
