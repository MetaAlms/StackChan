#!/usr/bin/env python3
"""Actual-source check for the DTLS short-probe repair (U1-0 / D3-1 + D3-2).

Registered with the offline check controller, which runs it inside a read-only
snapshot of the declared inputs with cwd at the snapshot root and no subprocess
or network access. It therefore reads only the files listed as inputs and never
touches the live workspace, the ledger, or the ESP-IDF tree.

What it verifies, on the real source that the firmware compiles:

  D3-1  transport classification goes through the public Mbed TLS macros
        (STREAM=0 / DATAGRAM=1 in ssl.h:301-302) and never through bare numeric
        literals; a configuration known on its own can never classify a call, so
        an unknown identity stays unknown; the SRTP profile is read only after
        that DATAGRAM handshake completed.

  D3-2  `mbedtls_ssl_conf_transport` is handled as returning void, the real
        release symbols are `mbedtls_ssl_config_free` / `mbedtls_ssl_free` (the
        non-existent `mbedtls_ssl_conf_free` appears nowhere), updating an
        existing configuration's transport is recorded instead of early
        returned, every wrapper preserves the real return value and the errno
        the real call produced, and free / Arm actually terminate identity.

  SANITY the linker-wrap list in main/CMakeLists.txt matches the wrappers that
        are actually defined, so "wrapper defined" cannot drift from "wrapper
        requested".

The complementary compile-time and behavioural evidence (the real local Mbed
TLS headers, and the final ELF callsites) is produced outside this controller:
see firmware/tests/run_dtls_probe_offline_check.sh and the D3 report.
"""

import pathlib
import re
import sys

CORE = "firmware/main/hal/webrtc/dtls_probe_core.h"
PROBE = "firmware/main/hal/webrtc/dtls_short_probe.cc"
CMAKE = "firmware/main/CMakeLists.txt"
PUBLIC_HEADER = "firmware/main/hal/webrtc/dtls_short_probe.h"

# Ground truth recorded from this machine's Mbed TLS 3.6.5 (ssl.h:301-302).
# Asserted so a silent header change cannot quietly re-invert the classification.
TRANSPORT_STREAM = 0
TRANSPORT_DATAGRAM = 1

failures = []


def fail(message):
    failures.append(message)


def read(relative):
    path = pathlib.Path(relative)
    if not path.is_file():
        fail(f"missing declared input: {relative}")
        return ""
    return path.read_text(encoding="utf-8")


def require(condition, message):
    if not condition:
        fail(message)


def compact(text):
    """Strip comments and collapse whitespace, keeping code semantics."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r"\s+", " ", text)


def function_body(text, signature):
    """Return the brace-balanced body of the first function matching signature."""
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    return ""


core = read(CORE)
probe = read(PROBE)
cmake = read(CMAKE)
public_header = read(PUBLIC_HEADER)
core_code = compact(core)
probe_code = compact(probe)
# CMake comments may name the removed symbol as documentation; only the real
# --wrap= arguments are evidence, so prose is stripped before inspecting them.
cmake_code = re.sub(r"#[^\n]*", " ", cmake)

# ---------------------------------------------------------------- D3-1 ------

require("MBEDTLS_SSL_TRANSPORT_STREAM" in core,
        "core must use the public MBEDTLS_SSL_TRANSPORT_STREAM macro")
require("MBEDTLS_SSL_TRANSPORT_DATAGRAM" in core,
        "core must use the public MBEDTLS_SSL_TRANSPORT_DATAGRAM macro")
require(re.search(r"kTransportStream\s*=\s*MBEDTLS_SSL_TRANSPORT_STREAM", core_code),
        "kTransportStream must be derived from MBEDTLS_SSL_TRANSPORT_STREAM")
require(re.search(r"kTransportDatagram\s*=\s*MBEDTLS_SSL_TRANSPORT_DATAGRAM", core_code),
        "kTransportDatagram must be derived from MBEDTLS_SSL_TRANSPORT_DATAGRAM")
require(f"// {TRANSPORT_DATAGRAM}" in core and f"// {TRANSPORT_STREAM}" in core,
        "core must record the local macro values (STREAM=0 / DATAGRAM=1) next to "
        "the constants")

# The exact b45aaa7 defect: comparing a transport against a bare 0/1.
for name, code in (("dtls_probe_core.h", core_code), ("dtls_short_probe.cc", probe_code)):
    bare = re.findall(r"\b(?:transport|tr|trans)\s*==\s*[01]\b", code)
    require(not bare, f"{name} compares transport against bare literals: {bare}")

# Identity must come from a successful setup, and a configuration alone must
# never classify a call.
classify = compact(function_body(core, "inline Identity ClassifyLocked("))
require(classify, "ClassifyLocked must be present in the core")
if classify:
    ctx_at = classify.find("st.ctxs")
    setups_at = classify.find("st.setups")
    require(ctx_at >= 0, "ClassifyLocked must consult the context registration table")
    require("UnknownContext" in classify,
            "ClassifyLocked must report an unregistered context as unknown")
    require(setups_at < 0 or ctx_at < setups_at,
            "ClassifyLocked must require the context registration before consulting "
            "the setup table: a configuration alone must never classify a call")
    require("StaleGeneration" in classify,
            "ClassifyLocked must reject a registration from an earlier generation")

gate = compact(function_body(core, "inline bool ShouldReadProfile("))
require(gate, "ShouldReadProfile must be present in the core")
if gate:
    require("Identity::Datagram" in gate,
            "the profile gate must require a DATAGRAM identity")
    require(re.search(r"rc\s*==\s*0", gate),
            "the profile gate must require a successful handshake return")
    require("handshake_over" in gate,
            "the profile gate must require the completed handshake")

handshake = compact(function_body(core, "inline int WrapHandshake("))
require(handshake, "WrapHandshake must be present in the core")
if handshake:
    require("ShouldReadProfile" in handshake,
            "WrapHandshake must read the profile only through the gate")
    require("st.ctxs" not in handshake,
            "WrapHandshake must not re-implement classification")
    # F1: this call's generation is pinned at entry and re-checked before the
    # profile is read and again at commit, so a call that outlives an Arm()
    # writes nothing into the new report. (The behavioural proof for this lives
    # in dtls_probe_offline_check.cc scenario 9, run via the controller.)
    require("st.generation" in handshake,
            "WrapHandshake must pin the live generation at entry")
    require(handshake.count("IsCurrentGeneration(generation)") >= 2,
            "WrapHandshake must re-check the pinned generation after the real call "
            "and before reading the profile")
    require("st.generation != generation" in handshake,
            "WrapHandshake must re-check the generation again under the lock at commit")
    # F2: no front-loaded observation may change the real call's entry errno.
    pre_real = handshake.split("__real_mbedtls_ssl_handshake")[0]
    require(re.search(r"errno\s*=\s*entry_errno\s*;", pre_real),
            "WrapHandshake must restore the caller's errno immediately before the "
            "real call")

require("setup_overflow" in core_code and "ctx_overflow" in core_code,
        "both fixed tables must report overflow explicitly")

# ---------------------------------------------------------------- D3-2 ------

require(re.search(r"void\s+__real_mbedtls_ssl_conf_transport\s*\(", core_code),
        "__real_mbedtls_ssl_conf_transport must be declared void (ssl.h:2143)")
require(re.search(r"void\s+__real_mbedtls_ssl_config_free\s*\(", core_code),
        "the release symbol is mbedtls_ssl_config_free (ssl.h:5741)")
require(re.search(r"void\s+__real_mbedtls_ssl_free\s*\(", core_code),
        "mbedtls_ssl_free must also be wrapped (ssl.h:5569)")
require("mbedtls_ssl_conf_free" not in core_code,
        "core must not reference the non-existent mbedtls_ssl_conf_free")
require("mbedtls_ssl_conf_free" not in probe_code,
        "probe must not reference the non-existent mbedtls_ssl_conf_free")
require("mbedtls_ssl_conf_free" not in cmake_code,
        "CMakeLists must not wrap the non-existent mbedtls_ssl_conf_free")

# Updating an existing configuration's transport must be recorded, not dropped.
register = compact(function_body(core, "inline void RegisterSetupLocked("))
require(register, "RegisterSetupLocked must be present in the core")
if register:
    existing = register.find("st.setups[i].conf == conf")
    require(existing >= 0, "RegisterSetupLocked must detect an existing configuration")
    if existing >= 0:
        branch = register[existing:existing + 400]
        require("transport_updates" in branch,
                "an updated transport must be recorded (transport_updates), so "
                "conf_transport on an existing configuration cannot early-return")

# errno discipline and return passthrough in every wrapper body.
wrappers = {
    "WrapHandshake": "inline int WrapHandshake(",
    "WrapConfTransport": "inline void WrapConfTransport(",
    "WrapConfigDefaults": "inline int WrapConfigDefaults(",
    "WrapSetup": "inline int WrapSetup(",
    "WrapConfigFree": "inline void WrapConfigFree(",
    "WrapSslFree": "inline void WrapSslFree(",
}
for name, signature in wrappers.items():
    body = compact(function_body(core, signature))
    require(body, f"{name} must be present in the core")
    if not body:
        continue
    require("entry_errno" in body, f"{name} must save the caller's errno before the real call")
    require("real_errno" in body,
            f"{name} must save errno immediately after the real call")
    require(re.search(r"errno\s*=\s*real_errno\s*;", body),
            f"{name} must restore the errno the real call produced")

for name in ("WrapConfigDefaults", "WrapSetup"):
    body = compact(function_body(core, f"inline int {name}("))
    require(re.search(r"return\s+rc\s*;", body),
            f"{name} must forward the real return value unchanged")

# Identity must actually end: free and Arm.
require("ForgetContextLocked" in compact(function_body(core, "inline void WrapSslFree(")),
        "mbedtls_ssl_free must clear the context registration")
require("ForgetSetupLocked" in compact(function_body(core, "inline void WrapConfigFree(")),
        "mbedtls_ssl_config_free must clear the setup registration")
arm = compact(function_body(core, "inline void Arm("))
require("ClearIdentityLocked" in arm,
        "Arm must clear every registration so no old identity survives a generation")
require("generation" in arm, "Arm must advance the generation")

# The device translation unit: real `void` wrapper, graceful delegation.
require(re.search(r'extern\s+"C"\s+void\s+__wrap_mbedtls_ssl_conf_transport\s*\(', probe_code),
        "__wrap_mbedtls_ssl_conf_transport must be a void function")
require(re.search(r'extern\s+"C"\s+void\s+__wrap_mbedtls_ssl_config_free\s*\(', probe_code),
        "__wrap_mbedtls_ssl_config_free must be defined")
require(re.search(r'extern\s+"C"\s+void\s+__wrap_mbedtls_ssl_free\s*\(', probe_code),
        "__wrap_mbedtls_ssl_free must be defined")
require("__wrap_mbedtls_ssl_conf_free" not in probe_code,
        "__wrap_mbedtls_ssl_conf_free must be gone")
require("mbedtls_ssl_context_get_config" in probe_code,
        "the handshake wrapper must pass the live configuration identity")
require("dtls_probe_core.h" in public_header,
        "the public probe header must include the shared core")

# --------------------------------------------------------------- SANITY -----

for symbol in ("mbedtls_ssl_handshake", "mbedtls_ssl_setup", "mbedtls_ssl_config_free",
               "mbedtls_ssl_free", "mbedtls_ssl_config_defaults",
               "mbedtls_ssl_conf_transport"):
    require(f"--wrap={symbol}" in cmake_code, f"CMakeLists must wrap {symbol}")

defined = set(re.findall(r'__wrap_([A-Za-z0-9_]+)\s*\(', probe_code))
wrapped = set(re.findall(r"--wrap=([A-Za-z0-9_]+)", cmake_code))
missing = sorted(defined - wrapped)
require(not missing, f"wrappers defined but never --wrap'ed: {missing}")
unbacked = sorted(s for s in wrapped
                  if s.startswith("mbedtls_") and s not in defined)
require(not unbacked, f"mbedtls symbols --wrap'ed without a wrapper definition: {unbacked}")

if failures:
    print("DTLS_PROBE_ACTUAL_SOURCE_FAIL")
    for item in failures:
        print(f"  - {item}")
    sys.exit(1)

print("DTLS_PROBE_ACTUAL_SOURCE_PASS")
print(f"  wrappers defined={len(defined)} wrapped={len(wrapped)}")
print(f"  local transport ground truth: STREAM={TRANSPORT_STREAM} "
      f"DATAGRAM={TRANSPORT_DATAGRAM}")
