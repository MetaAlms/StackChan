#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

SESSION_RUN_CXX="${CXX:-c++}"
SESSION_RUN_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SESSION_RUN_FIRMWARE_DIR="$(cd "$SESSION_RUN_TEST_DIR/.." && pwd)"
SESSION_RUN_ARTIFACT_DIR="${SESSION_RUN_CONTROL_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
SESSION_RUN_ARTIFACT="$SESSION_RUN_ARTIFACT_DIR/session_run_control_check"
mkdir -p "$SESSION_RUN_ARTIFACT_DIR"

# Compilation is outside the offline controller. Execute the resulting binary
# through the task's original ledger; --build-only never executes the check.
"$SESSION_RUN_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I "$SESSION_RUN_FIRMWARE_DIR/main/hal/webrtc" \
    "$SESSION_RUN_TEST_DIR/session_run_control_check.cc" \
    -o "$SESSION_RUN_ARTIFACT"

echo "compile: OK ($SESSION_RUN_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
