#!/usr/bin/env python3
"""Verify one U1 comparison arm against its own build outputs.

Run once per arm (1.5.5 and 1.5.7) over that arm's *archived* artefacts, so the
evidence describes the binaries that were actually built rather than whatever
happens to be in the shared `managed_components/` afterwards. That matters
because both arms use the same generated component directory: rebuilding for the
second arm overwrites the first arm's library.

This is a build-output inspection, not an offline check: the ELF and map are far
larger than the check controller's per-input limit, and it reads the live tree
rather than a frozen snapshot. It is therefore reported separately from the
controller checks.

What it establishes:

  * the peer version and component_hash the arm was actually resolved from, and
    the SHA-256 of the esp32s3 peer library that went into the link;
  * that the six DTLS observation wrappers are present in the final ELF, and -
    from the linker's Cross Reference Table - which objects really call them,
    because "the wrapper has a definition" is not evidence that it runs;
  * which symbols the peer library requires but does not define, so the two arms
    can be diffed and any newly required symbol can be shown to be implemented
    in the final ELF rather than merely assumed to exist.

Read-only: it prints a report and optionally writes one JSON file.
"""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

WRAPPERS = [
    "__wrap_mbedtls_ssl_handshake",
    "__wrap_mbedtls_ssl_setup",
    "__wrap_mbedtls_ssl_conf_transport",
    "__wrap_mbedtls_ssl_config_defaults",
    "__wrap_mbedtls_ssl_config_free",
    "__wrap_mbedtls_ssl_free",
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_tool(name):
    """Prefer the toolchain that matches the compiler ESP-IDF 5.5.4 selects."""
    env = pathlib.Path.home() / ".espressif/tools/xtensa-esp-elf"
    candidates = sorted(env.glob(f"*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-{name}"))
    preferred = [c for c in candidates if "14.2.0" in str(c)]
    for candidate in (preferred + candidates):
        if candidate.is_file():
            return str(candidate)
    return shutil.which(f"xtensa-esp32s3-elf-{name}")


def nm_symbols(nm, path, undefined_only=False):
    cmd = [nm] + (["-u"] if undefined_only else []) + [path]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        symbols.add(parts[-1])
    return symbols


def lock_peer_block(lock_path):
    text = pathlib.Path(lock_path).read_text()
    match = re.search(r"\n  espressif/esp_peer:\n(.*?)(?=\n  [A-Za-z0-9_]+/|\Z)", text, re.S)
    if not match:
        return None
    block = match.group(1)
    # The component's own fields sit at four spaces; a dependency's fields are
    # nested one level deeper, so anchoring on four spaces skips them.
    version = re.search(r"^    version:\s*(\S+)", block, re.M)
    comp_hash = re.search(r"^    component_hash:\s*(\S+)", block, re.M)
    return {
        "version": version.group(1) if version else None,
        "component_hash": comp_hash.group(1) if comp_hash else None,
        "dependencies": re.findall(r"- name:\s*(\S+)", block),
    }


def yml_pin(yml_path):
    text = pathlib.Path(yml_path).read_text()
    match = re.search(r"espressif/esp_peer:\s*\n(?:\s+.*\n)*?\s+version:\s*(\S+)", text)
    return match.group(1) if match else None


def map_wrapper_callers(map_path):
    """Objects that reference each wrapper, per the linker's xref table."""
    lines = pathlib.Path(map_path).read_text(errors="replace").splitlines()
    start = next((i for i, l in enumerate(lines) if l.strip() == "Cross Reference Table"), None)
    if start is None:
        return {}
    refs, current = {}, None
    for line in lines[start:]:
        if line and not line.startswith(" "):
            current = line.split()[0]
        elif current and line.strip():
            refs.setdefault(current, []).append(line.strip())
    out = {}
    for w in WRAPPERS:
        objects = set()
        for entry in refs.get(w, []):
            m = re.search(r"\(([^)]+)\)", entry)
            objects.add(m.group(1) if m else entry)
        out[w] = sorted(objects)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", required=True, help="label, e.g. arm-a-esp_peer-1.5.5")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--peer-lib", required=True)
    ap.add_argument("--lock", required=True)
    ap.add_argument("--component-yml", required=True)
    ap.add_argument("--json-out")
    args = ap.parse_args()

    nm = find_tool("nm")
    if not nm:
        print("FAIL: xtensa-esp32s3-elf-nm not found", file=sys.stderr)
        return 2

    report = {"arm": args.arm, "checks": {}, "failures": []}

    def fail(msg):
        report["failures"].append(msg)

    pin = yml_pin(args.component_yml)
    block = lock_peer_block(args.lock)
    lib_hash = sha256(args.peer_lib)
    report["checks"]["peer_pin_in_idf_component_yml"] = pin
    report["checks"]["peer_lock"] = block
    report["checks"]["peer_lib_sha256"] = lib_hash
    print(f"arm                     : {args.arm}")
    print(f"idf_component.yml pin   : {pin}")
    if block:
        print(f"lock resolved version   : {block['version']}")
        print(f"lock component_hash     : {block['component_hash']}")
        print(f"lock dependencies       : {', '.join(block['dependencies']) or '(none)'}")
    print(f"esp32s3 peer lib sha256 : {lib_hash}")

    elf_defined = nm_symbols(nm, args.elf)
    present = [w for w in WRAPPERS if w in elf_defined]
    missing = [w for w in WRAPPERS if w not in elf_defined]
    report["checks"]["wrappers_defined_in_elf"] = present
    report["checks"]["wrappers_missing_from_elf"] = missing
    print(f"wrappers in ELF         : {len(present)}/{len(WRAPPERS)}"
          + (f"  MISSING={missing}" if missing else ""))
    if missing:
        fail(f"wrappers missing from the final ELF: {missing}")

    callers = map_wrapper_callers(args.map)
    report["checks"]["wrapper_callers"] = callers
    print("wrapper call sites (linker Cross Reference Table):")
    for w in WRAPPERS:
        objs = callers.get(w) or []
        print(f"  {w:40s} <- {', '.join(objs) if objs else 'NO CALLER'}")
        if not objs:
            fail(f"{w} is defined but has no caller in the final link")

    needed = sorted(s for s in nm_symbols(nm, args.peer_lib, undefined_only=True) if s and s != "U")
    report["checks"]["peer_lib_undefined_symbols"] = needed
    report["checks"]["peer_lib_symbols_not_in_elf"] = sorted(s for s in needed if s not in elf_defined)
    print(f"peer lib undefined syms : {len(needed)}")
    print(f"  resolved in this ELF  : {len(needed) - len(report['checks']['peer_lib_symbols_not_in_elf'])}")

    if args.json_out:
        pathlib.Path(args.json_out).write_text(json.dumps(report, indent=2, sort_keys=True))
        print(f"report written          : {args.json_out}")

    print(f"RESULT: {'PASS' if not report['failures'] else 'FAIL'}")
    for f in report["failures"]:
        print(f"  - {f}")
    return 0 if not report["failures"] else 1


if __name__ == "__main__":
    sys.exit(main())
