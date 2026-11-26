#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

CAPTURE_LIFECYCLE_CXX="${CXX:-c++}"
CAPTURE_LIFECYCLE_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPTURE_LIFECYCLE_FIRMWARE_DIR="$(cd "$CAPTURE_LIFECYCLE_TEST_DIR/.." && pwd)"
CAPTURE_LIFECYCLE_ARTIFACT_DIR="${CAPTURE_LIFECYCLE_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
CAPTURE_LIFECYCLE_ARTIFACT="$CAPTURE_LIFECYCLE_ARTIFACT_DIR/capture_lifecycle_check"
mkdir -p "$CAPTURE_LIFECYCLE_ARTIFACT_DIR"

# Compilation is outside the controller. Never execute here: the task's original
# ledger owns the offline check budget and immutable execution inputs.
"$CAPTURE_LIFECYCLE_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    -I "$CAPTURE_LIFECYCLE_FIRMWARE_DIR/main/hal/webrtc" \
    "$CAPTURE_LIFECYCLE_TEST_DIR/capture_lifecycle_check.cc" \
    -o "$CAPTURE_LIFECYCLE_ARTIFACT"

echo "compile: OK ($CAPTURE_LIFECYCLE_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
