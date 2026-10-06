#!/usr/bin/env bash
# Store / read the Bailian API key in the macOS Keychain.
#
# Why: passing --api-key on the command line leaks the secret three ways — into
# shell history, into the process list (visible to ps), and into whatever
# dotfile you exported it from. The Keychain keeps it encrypted at rest and
# releases it only to a process the user authorises.
#
# This only protects the Mac side. The ESP32 has no Keychain equivalent, so the
# firmware necessarily holds a credential. See docs/aliyun-credentials.md for
# why a short-lived token is the right long-term answer there.
#
# Usage:
#   tools/aliyun_keychain.sh set      # prompt for the key, store it (no echo)
#   tools/aliyun_keychain.sh get      # print the key (used by the probe)
#   tools/aliyun_keychain.sh show     # print a masked form, for confirmation
#   tools/aliyun_keychain.sh delete   # remove it
#   tools/aliyun_keychain.sh names    # show the service/account in use
#
# Env overrides (rarely needed):
#   STACKCHAN_BAILIAN_KEYCHAIN_SERVICE
#   STACKCHAN_BAILIAN_KEYCHAIN_ACCOUNT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTOCOL_PY="$SCRIPT_DIR/aliyun_omni/protocol.py"

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

require_macos() {
    [[ "$(uname -s)" == "Darwin" ]] || die "this helper requires macOS (security(1) is unavailable)"
}

# The names live in tools/aliyun_omni/protocol.py so the shell helper and the
# Python probe cannot drift apart. Importing it would need the package on
# sys.path, so the two constants are parsed instead.
read_names_from_protocol() {
    [[ -f "$PROTOCOL_PY" ]] || die "cannot find $PROTOCOL_PY"
    python3 - "$PROTOCOL_PY" <<'PY'
import ast, sys

want = {"KEYCHAIN_SERVICE", "KEYCHAIN_ACCOUNT"}
found = {}
with open(sys.argv[1]) as handle:
    tree = ast.parse(handle.read(), sys.argv[1])
for node in tree.body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id in want:
                found[target.id] = ast.literal_eval(node.value)
missing = want - found.keys()
if missing:
    sys.exit(f"missing constants in protocol.py: {sorted(missing)}")
print(found["KEYCHAIN_SERVICE"])
print(found["KEYCHAIN_ACCOUNT"])
PY
}

# macOS ships bash 3.2, which has no mapfile/readarray. Read the two lines with
# a plain while-loop so the helper runs on a stock system shell.
_names="$(read_names_from_protocol)"
SERVICE="$(printf '%s\n' "$_names" | sed -n '1p')"
ACCOUNT="$(printf '%s\n' "$_names" | sed -n '2p')"
[[ -n "$SERVICE" && -n "$ACCOUNT" ]] || die "failed to read Keychain names from protocol.py"
SERVICE="${STACKCHAN_BAILIAN_KEYCHAIN_SERVICE:-$SERVICE}"
ACCOUNT="${STACKCHAN_BAILIAN_KEYCHAIN_ACCOUNT:-$ACCOUNT}"

case "${1:-}" in
    set)
        require_macos
        # -s keeps the prompt silent: the key never reaches the screen, a log,
        # or the process table. It is deliberately not accepted as an argument.
        printf 'Paste the Bailian API key (input hidden), then press Return.\n'
        read -r -s -p 'API key: ' key
        printf '\n'
        [[ -n "$key" ]] || die "empty key, nothing stored"
        [[ "$key" == sk-* ]] || printf 'warning: key does not start with "sk-"; storing anyway\n' >&2
        # -U updates in place when the item already exists.
        security add-generic-password -s "$SERVICE" -a "$ACCOUNT" -w "$key" -U
        printf 'stored: service=%s account=%s\n' "$SERVICE" "$ACCOUNT"
        printf 'masked: %s...%s\n' "${key:0:6}" "${key: -4}"
        ;;

    get)
        require_macos
        security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w 2>/dev/null \
            || die "no key stored (service=$SERVICE account=$ACCOUNT); run: $0 set"
        ;;

    show)
        require_macos
        key="$(security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w 2>/dev/null)" \
            || die "no key stored (service=$SERVICE account=$ACCOUNT)"
        printf 'service=%s\naccount=%s\nmasked=%s...%s\nlength=%s\n' \
            "$SERVICE" "$ACCOUNT" "${key:0:6}" "${key: -4}" "${#key}"
        ;;

    names)
        printf 'service=%s\naccount=%s\nsource=%s\n' "$SERVICE" "$ACCOUNT" "$PROTOCOL_PY"
        ;;

    delete)
        require_macos
        if security delete-generic-password -s "$SERVICE" -a "$ACCOUNT" >/dev/null 2>&1; then
            printf 'deleted: service=%s account=%s\n' "$SERVICE" "$ACCOUNT"
        else
            printf 'nothing to delete (service=%s account=%s)\n' "$SERVICE" "$ACCOUNT"
        fi
        ;;

    *)
        sed -n '2,26p' "$0"
        exit 1
        ;;
esac
