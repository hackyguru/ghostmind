#!/usr/bin/env bash
# Build the Ghostmind guest into ./ghostmind.bin with the local risc0 toolchain
# (`rzup install rust` + cargo-risczero 3.0.5; no Docker needed).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE/build-local"
cargo run --release -- "$HERE"
