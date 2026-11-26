#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != "--build-only" ]]; then
    echo "usage: $0 --build-only (execute the binary through the original task ledger)" >&2
    exit 2
fi

MEDIA_ADMISSION_CXX="${CXX:-c++}"
MEDIA_ADMISSION_TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MEDIA_ADMISSION_FIRMWARE_DIR="$(cd "$MEDIA_ADMISSION_TEST_DIR/.." && pwd)"
MEDIA_ADMISSION_ARTIFACT_DIR="${MEDIA_ADMISSION_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
MEDIA_ADMISSION_ARTIFACT="$MEDIA_ADMISSION_ARTIFACT_DIR/media_admission_check"
mkdir -p "$MEDIA_ADMISSION_ARTIFACT_DIR"

# Compilation is outside the offline controller. Run only the resulting binary
# through the existing task ledger after binding its declared source inputs.
"$MEDIA_ADMISSION_CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
    -I "$MEDIA_ADMISSION_FIRMWARE_DIR/main/hal/webrtc" \
    "$MEDIA_ADMISSION_TEST_DIR/media_admission_check.cc" \
    -o "$MEDIA_ADMISSION_ARTIFACT"
echo "compile: OK ($MEDIA_ADMISSION_ARTIFACT)"
echo "build-only: skipping execution; use the original task ledger"
