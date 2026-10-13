#!/usr/bin/env python3
"""Actual-source wiring check for the U1-S short mode corrections.

Registered with the offline check controller, which runs it inside a read-only
snapshot of the declared inputs with cwd at the snapshot root and no subprocess
or network access.

Why this exists next to `m1_short_offline_check.cc`: that harness drives the real
`m1_short::Sequencer`, but it is a *model* test. It cannot observe whether the
device adapter actually initialises the media clock, where the window is settled
relative to the task join, or which evidence label the verdict prints. Those five
corrections from `docs/webrtc-media-u1-preflight.md` are wiring facts, so they are
asserted here against the real source, and the behavioural harness is explicitly
scoped to the plan and interval arithmetic only.

Checked:
  1. `RunShortDiag` initialises the media clock exactly once, before the step
     loop, and does not restart the RTP observation window.
  2. The three-clip M1 acceptance verdict is compiled only in the full mode, and
     the short mode reports its own bounded result instead.
  3. The clip-interval evidence is named and described as write activity, not as
     proof that non-silent audio reached the server; the fixture's lead silence
     is stated and matches `fixture/manifest.json`.
  4. Interval boundaries and activity are separate, so an interval with no new
     packets cannot inherit a range and a missing start boundary is not reported
     as "no packet observed".
  5. `WaitForClipAsr` snapshots partial VAD/item evidence on both no-result
     paths without consuming the pending turn flags.
"""

import json
import pathlib
import re
import sys

M1 = "firmware/main/hal/webrtc/webrtc_m1.cc"
PLAN = "firmware/main/hal/webrtc/m1_short_plan.h"
MANIFEST = "firmware/main/hal/webrtc/fixture/manifest.json"

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


def compact(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r"\s+", " ", text)


def function_body(text, signature):
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:i + 1]
    return ""


m1 = read(M1)
plan = read(PLAN)
manifest = json.loads(read(MANIFEST) or "{}")
m1_code = compact(m1)
plan_code = compact(plan)

# ---------------------------------------------------------------- 1. clock ---
short = compact(function_body(m1, "void RunShortDiag()"))
require(short, "RunShortDiag must be present")
if short:
    require(short.count("g_clock.Arm()") == 1,
            "RunShortDiag must initialise the media clock exactly once "
            f"(found {short.count('g_clock.Arm()')})")
    require("rtp_probe::Arm()" not in short,
            "RunShortDiag must not restart the RTP observation window")
    arm_at = short.find("g_clock.Arm()")
    loop_at = short.find("while (seq.current()")
    require(loop_at < 0 or (arm_at >= 0 and arm_at < loop_at),
            "the media clock must be armed before the step loop, not inside it")
# The full mode still arms its own clock; the two paths must not be confused.
require("g_clock.Arm()" in compact(function_body(m1, "void RunFullM1()")),
        "RunFullM1 must keep its own media clock initialisation")

# -------------------------------------------------------------- 2. verdict ---
verdict_at = m1.find("R2-5: VERDICT computed from the real acceptance conditions")
require(verdict_at > 0, "the M1 acceptance verdict block must still exist")
if verdict_at > 0:
    # The guard sits immediately after the explanatory comment, so look forward
    # from the comment rather than backwards from it.
    guard = m1.find("#if CONFIG_STACKCHAN_WEBRTC_M1_SHORT_DIAG", verdict_at)
    require(guard > 0, "the acceptance block must be guarded by the short-mode switch")
    if guard > 0:
        alt = m1.find("#else", guard)
        require(alt > guard, "the guard must have an #else branch for the full mode")
        between = m1[guard:alt] if alt > guard else ""
        require("FINAL VERDICT not computed" in between,
                "the short branch must state that the M1 verdict is not computed")
        after = m1.find("#endif  // !CONFIG_STACKCHAN_WEBRTC_M1_SHORT_DIAG", guard)
        require(after > guard,
                "the acceptance guard must be closed after the verdict block")
require("SHORT DIAG VERDICT" in m1_code,
        "the short mode must report its own bounded verdict")
require("M1 coverage" in m1_code and "NOT covered by this mode" in m1_code,
        "the short verdict must state that it does not cover M1")

# ------------------------------------------------------ 3. evidence naming ---
require("HasSpeechEvidence" not in m1_code and "HasSpeechEvidence" not in plan_code,
        "the clip-interval predicate must not be called speech evidence")
require("HasClipWriteActivity" in m1_code and "HasClipWriteActivity" in plan_code,
        "the clip-interval predicate must be named clip write activity")
require("clip write activity" in m1_code,
        "the short verdict must label the evidence as clip write activity")
require("does NOT show" in m1_code or "not shown" in m1_code,
        "the verdict must state what the write activity does not show")
lead = manifest.get("lead_silence_ms")
require(lead == 500, f"fixture manifest lead_silence_ms changed: {lead}")
require(f"kFixtureLeadSilenceMs = {lead}" in m1_code,
        "kFixtureLeadSilenceMs must match the fixture manifest's lead_silence_ms")
require("kFixtureLeadSilenceMs" in m1_code,
        "the verdict must reference the fixture's lead silence")

# ------------------------------------------------- 4. boundary vs activity ---
for field in ("has_start_boundary", "has_end_boundary", "has_new_activity"):
    require(field in plan_code, f"Interval must carry {field}")
require("have_range" not in plan_code,
        "the conflated have_range field must be gone")
has_range = compact(function_body(plan, "inline bool HasRange("))
require(has_range, "HasRange must exist")
if has_range:
    require("has_new_activity" in has_range and "has_end_boundary" in has_range,
            "HasRange must require both new activity and an end boundary")
    require("has_start_boundary" not in has_range,
            "HasRange must not require a start boundary: the leading silence has none")
# The log must not call a no-activity interval "no packet observed", and must
# not print a range for one.
log = compact(function_body(m1, "void LogShortInterval("))
require(log, "LogShortInterval must be present")
if log:
    require("no new media in this interval" in log,
            "an interval with no new packets must be reported as no new media")
    require("no packet observed" not in log,
            "the old 'no packet observed' wording must be gone")
    require("has_new_activity" in log,
            "the range must only be printed for an interval with new activity")
    require("no packet before this interval" in log,
            "a missing start boundary must be stated as such, not as absent packets")

# --------------------------------------------------- 5. partial VAD evidence --
wait = compact(function_body(m1, "bool WaitForClipAsr("))
require(wait, "WaitForClipAsr must be present")
if wait:
    require(wait.count("SnapshotTurnEvidence(r)") >= 2,
            "both no-result paths (link drop and timeout) must snapshot turn evidence")
snap = compact(function_body(m1, "void SnapshotTurnEvidence("))
require(snap, "SnapshotTurnEvidence must exist")
if snap:
    for field in ("turn_speech_started", "turn_speech_stopped", "turn_item_id",
                  "turn_transcript"):
        require(field in snap, f"SnapshotTurnEvidence must carry {field}")
    require("turn_completed = false" not in snap and "turn_failed = false" not in snap,
            "the snapshot must not consume the pending turn flags")

# ------------------------------------------------------- join + settle order --
take = m1.find("xSemaphoreTake(g_sender_done")
settle = m1.find("rtp_probe::SettleWindow()")
require(take > 0 and settle > take,
        "SettleWindow() must run after the sender task has been joined")

# ------------------------------------------------------------ 20 ms pin ------
require("CONFIG_STACKCHAN_WEBRTC_M1_FRAME_60MS" in m1 and "#error" in m1,
        "the short mode must fail the build on 60 ms frames rather than differ silently")

if failures:
    print("U1_SHORT_WIRING_FAIL")
    for item in failures:
        print(f"  - {item}")
    sys.exit(1)

print("U1_SHORT_WIRING_PASS")
print("  media clock armed once before the step loop; RTP window not restarted")
print("  three-clip acceptance compiled only in the full mode")
print("  clip-interval evidence labelled as write activity (lead silence stated)")
print("  interval boundaries and activity reported separately")
print("  partial turn evidence preserved on both no-result paths")
