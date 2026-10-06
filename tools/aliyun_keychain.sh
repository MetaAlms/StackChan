#!/usr/bin/env bash
# Store / read the Bailian API key in the macOS Keychain.
#
# Why: passing --api-key on the command line leaks the secret three ways - into
# shell history, into the process list (visible to `ps`), and into whatever
# dotfile you exported it from. The Keychain keeps it encrypted at rest and
# releases it only to a process the user authorises.
#
# Note this only protects the Mac side. The ESP32 has no Keychain equivalent, so
# the firmware necessarily holds a credential. See docs/aliyun-credentials.md
# for why a short-lived token is the right long-term answer there.
#
# Usage:
#   tools/aliyun_keychain.sh set      # prompt for the key, store it (no echo)
#   tools/aliyun_keychain.sh get      # print the key (used by the probe)
#   tools/aliyun_keychain.sh show     # print a masked form, for confirmation
#   tools/aliyun_keychain.sh delete   # remove it
#
# Env:
#   BAILIAN_KEYCHAIN_SERVICE  override the service name
#   BAILIAN_KEYCHAIN_ACCOUNT  override the account name

set -euo pipefail

SERVICE="${BAILIAN_KEYCHAIN_SERVICE:-bailian-dashscope-api-key}"
ACCOUNT="${BAILIAN_KEYCHAIN_ACCOUNT:-$(id -un)}"

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

require_macos() {
    [[ "$(uname -s)" == "Darwin" ]] || die "this helper requires macOS (security(1) is not available)"
}

case "${1:-}" in
    set)
        require_macos
        # -s makes the prompt silent: the key never appears on screen or in
        # any log. It is also deliberately not accepted as an argument.
        printf 'Paste the Bailian API key (input hidden), then press Return.\n'
        read -r -s -p 'API key: ' key
        printf '\n'
        [[ -n "$key" ]] || die "empty key, nothing stored"
        [[ "$key" == sk-* ]] || printf 'warning: key does not start with "sk-"; storing anyway\n' >&2
        # -U updates in place if the item already exists.
        security add-generic-password -s "$SERVICE" -a "$ACCOUNT" -w "$key" -U
        printf 'stored in Keychain: service=%s account=%s\n' "$SERVICE" "$ACCOUNT"
        printf 'masked: %s...%s\n' "${key:0:6}" "${key: -4}"
        ;;

    get)
        require_macos
        security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w 2>/dev/null \
            || die "no key in Keychain for service=$SERVICE account=$ACCOUNT (run: $0 set)"
        ;;

    show)
        require_macos
        key="$(security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w 2>/dev/null)" \
            || die "no key in Keychain for service=$SERVICE account=$ACCOUNT"
        printf 'service=%s\naccount=%s\nmasked=%s...%s\nlength=%s\n' \
            "$SERVICE" "$ACCOUNT" "${key:0:6}" "${key: -4}" "${#key}"
        ;;

    delete)
        require_macos
        security delete-generic-password -s "$SERVICE" -a "$ACCOUNT" >/dev/null 2>&1 \
            && printf 'deleted service=%s account=%s\n' "$SERVICE" "$ACCOUNT" \
            || printf 'nothing to delete for service=%s account=%s\n' "$SERVICE" "$ACCOUNT"
        ;;

    *)
        sed -n '2,25p' "$0"
        exit 1
        ;;
esac
