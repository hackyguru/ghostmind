#!/usr/bin/env bash
# Build the Ghostmind harness against the pinned LEZ wallet FFI and libsodium, then
# run it. The impl's generated `logos_sdk.h` is replaced by tests/stubs/, an
# in-process message bus, so two module instances can talk in one process.
#
#   tests/run.sh                      offline: Borsh layout, seeds, amounts, chat crypto
#   tests/run.sh deploy ../ghostmind-program/ghostmind.bin
#   tests/run.sh init | state | gas | register NAME BIO SPECIALTIES PRICE | …
#   tests/run.sh session T_DIR C_DIR [V_DIR]   therapist + client (+ verifier) end to end
#
# Live commands use $GHOSTMIND_DATA_DIR (default ./ghostmind-data) and the public testnet.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../src"
export PATH=/nix/var/nix/profiles/default/bin:$PATH
NIX="nix --extra-experimental-features nix-command --extra-experimental-features flakes"
LEZ_REV=1a003bfbfb89fcf987f62975cd08a401169e1b8c  # v0.3.1

# LEZ's flake builds with rust-overlay's "stable.latest" from its own lock,
# which predates the Rust 1.98 v0.3.1 needs; use a current rust-overlay.
FFI=$($NIX build --no-link --print-out-paths "github:logos-blockchain/logos-execution-zone?rev=$LEZ_REV#wallet" \
    --override-input rust-overlay github:oxalica/rust-overlay)
JSON=$($NIX build --no-link --print-out-paths --impure --expr '(import <nixpkgs> {}).nlohmann_json')
SDK=$($NIX build --no-link --print-out-paths "github:logos-co/logos-cpp-sdk")
SODIUM=$($NIX build --no-link --print-out-paths --impure --expr '(import <nixpkgs> {}).libsodium.dev')
SODIUM_LIB=$($NIX build --no-link --print-out-paths --impure --expr '(import <nixpkgs> {}).libsodium')

if [ ! -x "$HERE/harness" ] || [ -n "$(find "$SRC" "$HERE/harness.cpp" "$HERE/stubs" -newer "$HERE/harness" 2>/dev/null)" ]; then
    clang++ -std=c++17 -O1 -Wall -Wno-unused-function -o "$HERE/harness" "$HERE/harness.cpp" "$SRC/ghostmind_impl.cpp" \
        -I"$SRC" -I"$HERE/stubs" -I"$FFI/include" -I"$JSON/include" -I"$SDK/include" -I"$SODIUM/include" \
        -L"$FFI/lib" -lwallet_ffi -Wl,-rpath,"$FFI/lib" -L"$SODIUM_LIB/lib" -lsodium -Wl,-rpath,"$SODIUM_LIB/lib" \
        -framework Security -framework CoreFoundation -framework SystemConfiguration \
        2>&1 | grep -v "extern-c-compat\|typedef struct WalletHandle\|^ *[0-9]* |\|^ *| *\^\|warning generated\|warnings generated" || true
    [ -x "$HERE/harness" ] || { echo "harness did not build" >&2; exit 1; }
fi
[ $# -eq 0 ] && exec "$HERE/harness" codec
exec "$HERE/harness" "$@"
