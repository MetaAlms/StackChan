#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

CHANNEL_AUDIT_CXX="${CXX:-c++}"
CHANNEL_AUDIT_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHANNEL_AUDIT_FIRMWARE_DIR="$(cd "$CHANNEL_AUDIT_TEST_DIR/.." && pwd)"
CHANNEL_AUDIT_ARTIFACT_DIR="${CHANNEL_AUDIT_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
CHANNEL_AUDIT_ARTIFACT="$CHANNEL_AUDIT_ARTIFACT_DIR/channel_audit_check"
mkdir -p "$CHANNEL_AUDIT_ARTIFACT_DIR"

# Compilation is outside the offline controller. Run only the resulting binary
# through the existing task ledger after binding its declared source inputs.
"$CHANNEL_AUDIT_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I "$CHANNEL_AUDIT_FIRMWARE_DIR/main/hal/webrtc" \
    "$CHANNEL_AUDIT_TEST_DIR/channel_audit_check.cc" \
    -o "$CHANNEL_AUDIT_ARTIFACT"
echo "compile: OK ($CHANNEL_AUDIT_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
