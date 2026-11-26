#!/usr/bin/env bash
#
# Archive one U1 comparison arm from the current build outputs.
#
# Both arms use the same generated `managed_components/` directory, so building
# the second arm overwrites the first arm's peer library. Each arm must
# therefore be archived immediately after its own build, and the two-arm
# comparison must be made between archives - never against whatever happens to
# be in the tree afterwards.
#
# Usage:
#   archive_u1_arm.sh <arm-label> <peer-version>
#
# Writes to $DTLS_PROBE_ARTIFACT_DIR/u1-archive/<arm-label>/ :
#   stack-chan.elf/.map/.bin      final link products (symbol evidence lives here)
#   bin/                          a self-consistent flash directory: every path
#                                 referenced by flash_args / flasher_args.json
#                                 resolves inside bin/ (see A1 below)
#   component/                    peer manifest, changelog, esp32s3 library, hashes
#   dependencies.lock             resolved dependency set for this arm
#   idf_component.yml             the pin this arm was built from
#   sdkconfig.local-sensitive     full config; private archive only, never Git
#   FROZEN_SOURCE_FINGERPRINT.txt digest of the shared, frozen source set
#   ARM_VERIFY.json               output of verify_u1_arm.py
#
# A1 - flash-argument layout: `flash_args` and `flasher_args.json` are copied
# byte-for-byte from the build, and they name their inputs *relative to the
# directory they sit in*:
#
#     bootloader/bootloader.bin        stack-chan.bin
#     partition_table/partition-table.bin
#     ota_data_initial.bin             generated_assets.bin
#
# The archive must reproduce that directory shape inside `bin/`; flattening it
# (an earlier defect) left the parameters pointing at files that did not exist,
# so the archive could not be flashed on its own. `verify_u1_archive.py`
# re-checks every reference after the fact.
#
# A2 - permissions: the archive holds a full sdkconfig, which carries device
# credentials, so it is created under umask 077 and the credential-bearing file
# is forced to 0600.
#
# Nothing here is committed to Git: the firmware binaries and the full sdkconfig
# stay in the local private archive, and only a desensitised record goes into the
# repository.

set -euo pipefail

# Private by default: this archive contains a credential-bearing sdkconfig.
umask 077

ARM_LABEL="${1:?usage: archive_u1_arm.sh <arm-label> <peer-version>}"
PEER_VERSION="${2:?usage: archive_u1_arm.sh <arm-label> <peer-version>}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
ARCHIVE_ROOT="${DTLS_PROBE_ARTIFACT_DIR:-$HOME/.codex/dev-harness/artifacts/stackchan-webrtc-media}/u1-archive"
ARM_DIR="$ARCHIVE_ROOT/$ARM_LABEL"

# The frozen common source set. Both arms must produce the same digest; the
# archive script records it per arm so the RQ can show they matched.
SOURCES=(
    firmware/main/hal/webrtc/webrtc_m1.cc
    firmware/main/hal/webrtc/m1_short_plan.h
    firmware/main/hal/webrtc/dtls_probe_core.h
    firmware/main/hal/webrtc/dtls_short_probe.cc
    firmware/main/hal/webrtc/dtls_short_probe.h
    firmware/main/hal/webrtc/rtp_send_probe.cc
    firmware/main/hal/webrtc/rtp_send_probe.h
    firmware/main/hal/webrtc/webrtc_transport.cc
    firmware/main/hal/webrtc/webrtc_transport.h
    firmware/main/CMakeLists.txt
    firmware/main/Kconfig.projbuild
    firmware/main/hal/webrtc/fixture/manifest.json
    firmware/main/hal/webrtc/fixture/zh_1.pcm
    firmware/main/hal/webrtc/fixture/zh_2.pcm
    firmware/main/hal/webrtc/fixture/zh_3.pcm
)

rm -rf "$ARM_DIR"
mkdir -p "$ARM_DIR/bin" "$ARM_DIR/component"

cp "$FW/build/stack-chan.elf" "$FW/build/stack-chan.map" "$FW/build/stack-chan.bin" "$ARM_DIR/"

# bin/ must mirror the directory shape the flash parameters refer to: the
# parameters name `bootloader/bootloader.bin`, `stack-chan.bin` and
# `partition_table/partition-table.bin` relative to their own directory.
mkdir -p "$ARM_DIR/bin/bootloader" "$ARM_DIR/bin/partition_table"
cp "$FW/build/stack-chan.bin"                        "$ARM_DIR/bin/stack-chan.bin"
cp "$FW/build/bootloader/bootloader.bin"             "$ARM_DIR/bin/bootloader/bootloader.bin"
cp "$FW/build/partition_table/partition-table.bin"   "$ARM_DIR/bin/partition_table/partition-table.bin"
cp "$FW/build/ota_data_initial.bin" "$FW/build/generated_assets.bin" "$ARM_DIR/bin/"
cp "$FW/build/flash_args" "$FW/build/flasher_args.json" "$ARM_DIR/bin/"
cp "$FW/sdkconfig" "$ARM_DIR/sdkconfig.local-sensitive"

cp "$FW/dependencies.lock" "$ARM_DIR/dependencies.lock"
cp "$FW/main/idf_component.yml" "$ARM_DIR/idf_component.yml"
cp "$FW/managed_components/espressif__esp_peer/idf_component.yml" \
   "$ARM_DIR/component/esp_peer-idf_component.yml"
cp "$FW/managed_components/espressif__esp_peer/libs/esp32s3/libpeer_default.a" \
   "$ARM_DIR/component/libpeer_default-esp32s3.a"
for extra in CHECKSUMS.json CHANGELOG.md; do
    if [[ -f "$FW/managed_components/espressif__esp_peer/$extra" ]]; then
        cp "$FW/managed_components/espressif__esp_peer/$extra" "$ARM_DIR/component/$extra"
    fi
done

# Per-file hashes and the combined fingerprint over the frozen source set.
( cd "$REPO" && printf '%s\n' "${SOURCES[@]}" | xargs shasum -a 256 ) \
    > "$ARM_DIR/component/SOURCE_HASHES.txt"
FINGERPRINT=$(shasum -a 256 "$ARM_DIR/component/SOURCE_HASHES.txt" | cut -d' ' -f1)
echo "$FINGERPRINT" > "$ARM_DIR/FROZEN_SOURCE_FINGERPRINT.txt"

# Record which peer version the archive claims to be, and fail loudly if the
# build outputs disagree with the label.
RESOLVED=$(python3 - "$ARM_DIR/dependencies.lock" <<'PY'
import re, sys, pathlib
t = pathlib.Path(sys.argv[1]).read_text()
m = re.search(r"\n  espressif/esp_peer:\n(.*?)(?=\n  [A-Za-z0-9_]+/|\Z)", t, re.S)
block = m.group(1) if m else ""
v = re.search(r"^    version:\s*(\S+)", block, re.M)
print(v.group(1) if v else "?")
PY
)
if [[ "$RESOLVED" != "$PEER_VERSION" ]]; then
    echo "FAIL: archive label says $PEER_VERSION but the lock resolved $RESOLVED" >&2
    exit 2
fi

python3 "$HERE/verify_u1_arm.py" \
    --arm "$ARM_LABEL" \
    --elf "$ARM_DIR/stack-chan.elf" \
    --map "$ARM_DIR/stack-chan.map" \
    --peer-lib "$ARM_DIR/component/libpeer_default-esp32s3.a" \
    --lock "$ARM_DIR/dependencies.lock" \
    --component-yml "$ARM_DIR/idf_component.yml" \
    --json-out "$ARM_DIR/ARM_VERIFY.json" | tee "$ARM_DIR/ARM_VERIFY.txt"

# A2: the archive root and the credential-bearing config are private. The
# umbrella directory is forced to 0700 because it holds this arm's sdkconfig.
chmod 700 "$ARCHIVE_ROOT" "$ARM_DIR" "$ARM_DIR/bin" "$ARM_DIR/component"
chmod 600 "$ARM_DIR/sdkconfig.local-sensitive"

# A1 + A2 verified against the archive as written, not against the build tree.
python3 "$HERE/verify_u1_archive.py" --archive-root "$ARCHIVE_ROOT" \
    --arm "$ARM_LABEL" --json-out "$ARM_DIR/ARCHIVE_VERIFY.json" \
    | tee "$ARM_DIR/ARCHIVE_VERIFY.txt"

echo
echo "archived $ARM_LABEL (peer $RESOLVED) -> $ARM_DIR"
echo "frozen source fingerprint: $FINGERPRINT"
