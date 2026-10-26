#!/usr/bin/env bash
#
# Offline check build/run driver for the U1-S short comparison mode.
#
# Same split as the D3 probe check:
#
#   build  - a C++ compile that invokes a compiler as a subprocess. NOT covered
#            by the offline check controller, which may not fork. Reported
#            separately.
#   run    - executing the already-built binary, which does no forking and no
#            network, so it IS run through the original ledger controller
#            (check `u1-short-behaviour`, cache=never), which binds the artefact
#            and the input sources into one fingerprint.
#
# The binary is written outside the workspace because the controller sandbox
# denies file-read* on the workspace, so a binary living there could not be
# executed.
#
# The harness includes firmware/main/hal/webrtc/m1_short_plan.h - the same header
# the device firmware compiles - so the state machine under test is the real one.

set -euo pipefail

CXX="${CXX:-c++}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_FIRMWARE="$(cd "$HERE/.." && pwd)"
ARTIFACT_DIR="${DTLS_PROBE_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
ARTIFACT="$ARTIFACT_DIR/m1_short_offline_check"

BUILD_ONLY=0
if [[ "${1:-}" == "--build-only" ]]; then
    BUILD_ONLY=1
fi

mkdir -p "$ARTIFACT_DIR"

echo "compiler : $($CXX --version | head -1)"
echo "artifact : $ARTIFACT"
echo

# No Mbed TLS here: the short-mode plan is deliberately ESP-free and portable,
# so this check has a smaller toolchain surface than the DTLS probe check.
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I "$REPO_FIRMWARE/main/hal/webrtc" \
    "$HERE/m1_short_offline_check.cc" \
    -o "$ARTIFACT"

echo "compile: OK"
echo

if [[ "$BUILD_ONLY" == "1" ]]; then
    echo "build-only: skipping execution (run the artefact via the ledger controller)"
    exit 0
fi

"$ARTIFACT"
