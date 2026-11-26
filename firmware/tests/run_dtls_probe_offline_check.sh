#!/usr/bin/env bash
#
# Offline check build/run driver for the DTLS short-probe core (U1-0 / F1-F3).
#
# Two separable steps, deliberately kept separate:
#
#   build  - a C++ compile that invokes a compiler as a subprocess. This is NOT
#            covered by the offline check controller, which may not fork.
#   run    - executing the already-built binary. That step has no subprocess and
#            no network, so it IS run through the original ledger controller
#            (check `u1-d3-probe-behaviour`, cache=never), which binds the
#            artefact and the input sources into one fingerprint.
#
# The binary is written outside the workspace on purpose: the controller sandbox
# denies file-read* on the workspace, so a binary living there could not be
# executed. The default artefact directory is stable, outside the workspace and
# outside the denied ledger directory. Use --build-only for the build half.
#
# What it does:
#   1. compiles firmware/main/hal/webrtc/dtls_probe_core.h - the same header the
#      device firmware compiles - together with the harness, against the *real*
#      local Mbed TLS 3.6.5 headers from the pinned ESP-IDF v5.5.4 tree;
#   2. runs the resulting binary, which drives the probe's own wrapper entry
#      points and asserts the classification / lifecycle / generation / errno
#      behaviour.
#
# The stub in the harness is checked against the real headers at compile time:
# each __real_* declaration is compared with decltype(&mbedtls_ssl_X), so an
# upstream signature change (or a hardcoded transport number) fails the build
# instead of mis-classifying HTTPS as DTLS.

set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-5.5}"
CXX="${CXX:-c++}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_FIRMWARE="$(cd "$HERE/.." && pwd)"
ARTIFACT_DIR="${DTLS_PROBE_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}"
ARTIFACT="$ARTIFACT_DIR/dtls_probe_offline_check"

BUILD_ONLY=0
if [[ "${1:-}" == "--build-only" ]]; then
    BUILD_ONLY=1
fi

MBEDTLS_INC="$IDF_PATH/components/mbedtls/mbedtls/include"
SSL_H="$MBEDTLS_INC/mbedtls/ssl.h"

if [[ ! -f "$SSL_H" ]]; then
    echo "FAIL: local Mbed TLS headers not found at $SSL_H" >&2
    echo "      set IDF_PATH to the pinned ESP-IDF v5.5.4 tree" >&2
    exit 2
fi

# The device build enables MBEDTLS_SSL_DTLS_SRTP through
# components/mbedtls/port/include/mbedtls/esp_config.h (guarded by
# CONFIG_MBEDTLS_SSL_PROTO_DTLS). The default in-tree config leaves it off, so
# the host build turns on exactly that one option to match the device.
mkdir -p "$ARTIFACT_DIR"

echo "IDF_PATH      : $IDF_PATH"
echo "mbedtls ssl.h : $SSL_H"
echo "compiler      : $($CXX --version | head -1)"
echo "artifact      : $ARTIFACT"
echo

"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -DMBEDTLS_SSL_DTLS_SRTP \
    -I "$MBEDTLS_INC" \
    -I "$REPO_FIRMWARE/main/hal/webrtc" \
    "$HERE/dtls_probe_offline_check.cc" \
    -o "$ARTIFACT"

echo "compile: OK"
echo

if [[ "$BUILD_ONLY" == "1" ]]; then
    echo "build-only: skipping execution (run the artefact via the ledger controller)"
    exit 0
fi

"$ARTIFACT"
