#!/usr/bin/env python3
"""Actual-source contract check for the U1 arm archiver (Review A1 + A2).

Registered with the offline check controller, which runs it inside a read-only
snapshot of the declared inputs with cwd at the snapshot root and no subprocess
or network access.

Why this exists next to `verify_u1_archive.py`: that validator inspects the real
archive, which contains a 67 MB ELF, a 32 MB map and multi-megabyte images - far
past the controller's 2 MB per-input and 10 MB snapshot limits. Its result is
therefore reported separately. What the controller *can* cover is the archiver's
contract, which is what this file asserts:

  A1  `archive_u1_arm.sh` must lay out `bin/` in the shape the copied
      `flash_args` / `flasher_args.json` actually name
      (`bootloader/bootloader.bin`, `stack-chan.bin`,
      `partition_table/partition-table.bin`), must not leave the flattened
      variants behind, and must run the archive validator afterwards.
      `verify_u1_archive.py` must resolve references from *both* parameter files
      and must compare the app image reachable through the parameters against
      the arm's recorded `stack-chan.bin`.

  A2  `archive_u1_arm.sh` must create the archive under `umask 077` and force
      the archive root to 0700 and the credential-bearing config to 0600; the
      validator must check those modes. No credential value is read anywhere.
"""

import pathlib
import re
import sys

ARCHIVER = "firmware/tests/archive_u1_arm.sh"
VALIDATOR = "firmware/tests/verify_u1_archive.py"

failures = []


def require(condition, message):
    if not condition:
        failures.append(message)


def read(relative):
    path = pathlib.Path(relative)
    if not path.is_file():
        failures.append(f"missing declared input: {relative}")
        return ""
    return path.read_text(encoding="utf-8")


sh = read(ARCHIVER)
vy = read(VALIDATOR)

# ---------------------------------------------------------------- A1 --------
require(re.search(r"mkdir -p \"\$ARM_DIR/bin/bootloader\" \"\$ARM_DIR/bin/partition_table\"", sh),
        "the archiver must create bin/bootloader and bin/partition_table")
require(re.search(r'cp "\$FW/build/stack-chan\.bin"\s+"\$ARM_DIR/bin/stack-chan\.bin"', sh),
        "the archiver must place the app image where flash_args looks for it (bin/stack-chan.bin)")
require(re.search(r'cp "\$FW/build/bootloader/bootloader\.bin"\s+"\$ARM_DIR/bin/bootloader/bootloader\.bin"', sh),
        "the archiver must place the bootloader at bin/bootloader/bootloader.bin")
require(re.search(r'cp "\$FW/build/partition_table/partition-table\.bin"\s+"\$ARM_DIR/bin/partition_table/partition-table\.bin"', sh),
        "the archiver must place the partition table at bin/partition_table/partition-table.bin")

# The flattened layout is the defect: it must not come back.
flat = re.findall(r'"\$ARM_DIR/bin/(bootloader\.bin|partition-table\.bin)"', sh)
require(not flat, f"the archiver must not write flat flash images into bin/: {flat}")

require("verify_u1_archive.py" in sh,
        "the archiver must run the archive validator over the archive it just wrote")
require(re.search(r"verify_u1_archive\.py.*--arm", sh, re.S),
        "the archiver must validate the specific arm it archived")

# The validator must resolve references from both parameter files and compare the
# app image the parameters select against the recorded link product.
require("flash_args" in vy and "flasher_args.json" in vy,
        "the validator must read both flash_args and flasher_args.json")
require("parse_flash_args" in vy and "parse_flasher_args" in vy,
        "the validator must parse both parameter formats")
require("0x20000" in vy,
        "the validator must identify the app image by its flash offset")
require("app_bin_recorded_sha256" in vy and "app_bin_from_params_sha256" in vy,
        "the validator must compare the parameter-selected app image with the "
        "arm's recorded stack-chan.bin")
require("does not exist in bin/" in vy,
        "the validator must fail a reference that does not resolve inside bin/")

# ---------------------------------------------------------------- A2 --------
require(re.search(r"^umask 077$", sh, re.M),
        "the archiver must set umask 077: the archive carries a credential-bearing sdkconfig")
require(re.search(r"chmod 600 \"\$ARM_DIR/sdkconfig\.local-sensitive\"", sh),
        "the archiver must force the credential-bearing config to 0600")
require(re.search(r"chmod 700 \"\$ARCHIVE_ROOT\" \"\$ARM_DIR\"", sh),
        "the archiver must force the archive root and arm directory to 0700")
require("stat.S_IMODE" in vy and "0o077" in vy,
        "the validator must check permission bits rather than assume them")
require("expected 0600" in vy and "expected 0700" in vy,
        "the validator must state the expected modes")

# The archiver must not reach outside this task's archive when tightening modes.
others = re.findall(r"chmod\s+[0-7]+\s+(\"[^\"]*\"|[^\s;]+)", sh)
outside = [o for o in others if "ARM_DIR" not in o and "ARCHIVE_ROOT" not in o]
require(not outside,
        f"the archiver must only chmod its own archive paths, saw: {outside}")

if failures:
    print("U1_ARCHIVE_CONTRACT_FAIL")
    for item in failures:
        print(f"  - {item}")
    sys.exit(1)

print("U1_ARCHIVE_CONTRACT_PASS")
print("  A1 bin/ mirrors the directory shape flash_args and flasher_args.json name")
print("  A1 the archiver runs the reference/hash validator over its own archive")
print("  A2 umask 077 plus 0700 root / 0600 credential-bearing config")
print("  A2 only this task's archive paths are chmod'ed")
