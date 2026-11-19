#!/usr/bin/env bash
# Builds the test WCLAPs into dist/<name>.wclap.tar.gz (OUT=<dir> to build elsewhere). The work is in build.cmake,
# which the plugin's CMake build runs on every platform. Needs LLVM (clang with the wasm32 target) and wasm-ld.
set -euo pipefail
cd "$(dirname "$0")"
CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
WASM_LD="${WASM_LD:-$(command -v wasm-ld || echo /opt/homebrew/opt/lld/bin/wasm-ld)}"
CLAP_INCLUDE="${CLAP_INCLUDE:-../../build/_deps/wclap-bridge-src/modules/clap/include}"
exec cmake -DCLANG="$CLANG" -DWASM_LD="$WASM_LD" -DCLAP_INCLUDE="$(cd "$CLAP_INCLUDE" && pwd)" -DOUT="$(mkdir -p "${OUT:-.}" && cd "${OUT:-.}" && pwd)" -P build.cmake
