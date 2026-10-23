#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

DECIMAL_LOG_CXX="${CXX:-c++}"
DECIMAL_LOG_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECIMAL_LOG_FIRMWARE_DIR="$(cd "$DECIMAL_LOG_TEST_DIR/.." && pwd)"
DECIMAL_LOG_ARTIFACT_DIR="${DECIMAL_LOG_VALUE_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
DECIMAL_LOG_ARTIFACT="$DECIMAL_LOG_ARTIFACT_DIR/decimal_log_value_check"
mkdir -p "$DECIMAL_LOG_ARTIFACT_DIR"

# Compilation is outside the offline controller. --build-only never executes
# the resulting binary; use the task's original ledger for execution.
"$DECIMAL_LOG_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I "$DECIMAL_LOG_FIRMWARE_DIR/main/hal/webrtc" \
    "$DECIMAL_LOG_TEST_DIR/decimal_log_value_check.cc" \
    -o "$DECIMAL_LOG_ARTIFACT"

echo "compile: OK ($DECIMAL_LOG_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
