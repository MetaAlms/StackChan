#!/usr/bin/env bash
#
# Offline check runner for the DTLS short-probe core (U1-0 / D3-1 + D3-2).
#
# This is a C++ compile/run check. It is deliberately NOT registered with the
# offline check controller: it invokes a compiler as a subprocess, which the
# controller's read-only snapshot scope does not cover. It is reported
# separately from the controller checks.
#
# What it does:
#   1. compiles firmware/main/hal/webrtc/dtls_probe_core.h - the same header the
#      device firmware compiles - together with the harness, against the *real*
#      local Mbed TLS 3.6.5 headers from the pinned ESP-IDF v5.5.4 tree;
#   2. runs the resulting binary, which drives the probe's own wrapper entry
#      points and asserts the classification / lifecycle / errno behaviour.
#
# The stub in the harness is checked against the real headers at compile time:
# a wrong bare signature (int for the void mbedtls_ssl_conf_transport) or a
# hardcoded transport number fails the build instead of mis-classifying HTTPS as
# DTLS.

set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-5.5}"
CXX="${CXX:-c++}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_FIRMWARE="$(cd "$HERE/.." && pwd)"

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
OUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dtls-probe-offline.XXXXXX")"
trap 'rm -rf "$OUT_DIR"' EXIT

echo "IDF_PATH      : $IDF_PATH"
echo "mbedtls ssl.h : $SSL_H"
echo "compiler      : $($CXX --version | head -1)"
echo "output dir    : $OUT_DIR"
echo

"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -DMBEDTLS_SSL_DTLS_SRTP \
    -I "$MBEDTLS_INC" \
    -I "$REPO_FIRMWARE/main/hal/webrtc" \
    "$HERE/dtls_probe_offline_check.cc" \
    -o "$OUT_DIR/dtls_probe_offline_check"

echo "compile: OK"
echo
"$OUT_DIR/dtls_probe_offline_check"
