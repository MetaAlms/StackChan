#!/usr/bin/env python3
"""Validate a U1 arm archive as it stands on disk.

Two defects are checked here, both of them properties of the *archive* rather
than of the firmware:

  A1  The copied `flash_args` / `flasher_args.json` name their inputs relative
      to the directory they live in (`bootloader/bootloader.bin`,
      `stack-chan.bin`, `partition_table/partition-table.bin`, ...). If the
      archive flattens those files, the parameters point at paths that do not
      exist and the archive cannot be flashed on its own. Every reference in
      both files is resolved and must exist, and the app image reachable through
      the parameters must be byte-identical to the arm's recorded `stack-chan.bin`.

  A2  The archive carries a full sdkconfig, which holds device credentials, so
      the archive root must be 0700 and the credential-bearing file 0600.

Scope and coverage boundary, stated rather than implied: this reads the
multi-megabyte link products, which is far beyond the offline check
controller's per-input limit (2 MB) and total snapshot limit (10 MB). It
therefore runs outside the controller and is reported separately. The
controller-side check (`check_u1_short_wiring.py`) asserts the *archiver's*
contract instead, which is the part that fits in a text snapshot.

Read-only apart from the optional JSON report it is asked to write.
"""

import argparse
import hashlib
import json
import os
import pathlib
import re
import stat
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_flash_args(path):
    """esptool's @file format: option lines plus `<offset> <path>` lines."""
    refs = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("--") or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 2 and parts[0].lower().startswith("0x"):
            refs.append((parts[0], parts[1]))
    return refs


def parse_flasher_args(path):
    data = json.loads(path.read_text())
    refs = list(data.get("flash_files", {}).items())
    return refs, data


def check_arm(arm_dir, failures, report):
    name = arm_dir.name
    bin_dir = arm_dir / "bin"
    entry = {"arm": name, "references": [], "problems": []}

    def fail(msg):
        failures.append(f"{name}: {msg}")
        entry["problems"].append(msg)

    if not bin_dir.is_dir():
        fail("bin/ is missing")
        report.append(entry)
        return

    flash_args = bin_dir / "flash_args"
    flasher = bin_dir / "flasher_args.json"
    if not flash_args.is_file():
        fail("bin/flash_args is missing")
    if not flasher.is_file():
        fail("bin/flasher_args.json is missing")

    # A1: every referenced path must resolve inside bin/.
    refs = []
    if flash_args.is_file():
        refs += [("flash_args", off, p) for off, p in parse_flash_args(flash_args)]
    if flasher.is_file():
        json_refs, _ = parse_flasher_args(flasher)
        refs += [("flasher_args.json", off, p) for off, p in json_refs]

    app_from_params = None
    for source, offset, rel in refs:
        target = (bin_dir / rel)
        resolved_ok = target.is_file() and not target.is_symlink()
        if not resolved_ok:
            fail(f"{source} references {rel} ({offset}) which does not exist in bin/")
        entry["references"].append({
            "source": source, "offset": offset, "path": rel,
            "exists": resolved_ok,
            "sha256": sha256(target) if resolved_ok else None,
        })
        if resolved_ok and offset == "0x20000":
            app_from_params = target

    # The app image the parameters select must be the arm's own recorded bin.
    recorded = arm_dir / "stack-chan.bin"
    if not recorded.is_file():
        fail("stack-chan.bin (recorded link product) is missing")
    elif app_from_params is None:
        fail("no 0x20000 app image referenced by the flash parameters")
    else:
        a, b = sha256(recorded), sha256(app_from_params)
        entry["app_bin_recorded_sha256"] = a
        entry["app_bin_from_params_sha256"] = b
        if a != b:
            fail("the app image referenced by the flash parameters differs from "
                 "the arm's recorded stack-chan.bin")

    # Link products that carry the evidence must still be present.
    for product in ("stack-chan.elf", "stack-chan.map"):
        if not (arm_dir / product).is_file():
            fail(f"{product} is missing")
    if not (arm_dir / "FROZEN_SOURCE_FINGERPRINT.txt").is_file():
        fail("FROZEN_SOURCE_FINGERPRINT.txt is missing")

    # A2: permission metadata only; no credential value is read or printed.
    sensitive = arm_dir / "sdkconfig.local-sensitive"
    if sensitive.is_file():
        mode = stat.S_IMODE(sensitive.stat().st_mode)
        entry["sensitive_mode"] = oct(mode)
        if mode & 0o077:
            fail(f"sdkconfig.local-sensitive is {oct(mode)}, expected 0600")
    entry["arm_dir_mode"] = oct(stat.S_IMODE(arm_dir.stat().st_mode))
    entry["bin_dir_mode"] = oct(stat.S_IMODE(bin_dir.stat().st_mode))
    if stat.S_IMODE(arm_dir.stat().st_mode) & 0o077:
        fail(f"{name} is {entry['arm_dir_mode']}, expected 0700")

    report.append(entry)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--archive-root", required=True)
    ap.add_argument("--arm", action="append", default=[],
                    help="arm directory name; repeatable (default: all under the root)")
    ap.add_argument("--json-out")
    args = ap.parse_args()

    root = pathlib.Path(args.archive_root)
    failures = []
    report = {"archive_root": str(root), "arms": [], "failures": []}

    if not root.is_dir():
        print(f"FAIL: archive root not found: {root}", file=sys.stderr)
        return 2

    # A2: the umbrella directory itself is private because it holds the configs.
    root_mode = stat.S_IMODE(root.stat().st_mode)
    report["archive_root_mode"] = oct(root_mode)
    if root_mode & 0o077:
        failures.append(f"archive root is {oct(root_mode)}, expected 0700")

    names = args.arm or sorted(d.name for d in root.iterdir() if d.is_dir())
    for name in names:
        arm_dir = root / name
        if not arm_dir.is_dir():
            failures.append(f"{name}: not a directory")
            continue
        check_arm(arm_dir, failures, report["arms"])

    report["failures"] = failures
    print(f"archive root : {root} ({report['archive_root_mode']})")
    for entry in report["arms"]:
        refs = entry.get("references", [])
        ok = sum(1 for r in refs if r["exists"])
        print(f"  {entry['arm']}")
        print(f"    flash references resolved : {ok}/{len(refs)}")
        if "app_bin_recorded_sha256" in entry:
            print(f"    app bin sha256            : {entry['app_bin_recorded_sha256']}")
            print(f"    app bin via parameters    : {entry['app_bin_from_params_sha256']}")
        if "sensitive_mode" in entry:
            print(f"    sensitive config mode     : {entry['sensitive_mode']}")
        for problem in entry.get("problems", []):
            print(f"    PROBLEM: {problem}")

    if args.json_out:
        pathlib.Path(args.json_out).write_text(json.dumps(report, indent=2, sort_keys=True))

    print(f"RESULT: {'PASS' if not failures else 'FAIL'}")
    for f in failures:
        print(f"  - {f}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
