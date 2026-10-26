#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

CONSOLE_COMMAND_CXX="${CXX:-c++}"
CONSOLE_COMMAND_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONSOLE_COMMAND_FIRMWARE_DIR="$(cd "$CONSOLE_COMMAND_TEST_DIR/.." && pwd)"
CONSOLE_COMMAND_ARTIFACT_DIR="${CONSOLE_COMMAND_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
CONSOLE_COMMAND_ARTIFACT="$CONSOLE_COMMAND_ARTIFACT_DIR/console_command_check"
mkdir -p "$CONSOLE_COMMAND_ARTIFACT_DIR"

# Compiler subprocesses are outside the offline controller. This driver only
# builds; execution belongs to the original task ledger with declared inputs.
"$CONSOLE_COMMAND_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I "$CONSOLE_COMMAND_FIRMWARE_DIR/main/hal/webrtc" \
    "$CONSOLE_COMMAND_TEST_DIR/console_command_check.cc" \
    -o "$CONSOLE_COMMAND_ARTIFACT"
echo "compile: OK ($CONSOLE_COMMAND_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
