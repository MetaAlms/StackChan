# 拟提交至 esp-webrtc-solution#208 的评论草稿

目标：<https://github.com/espressif/esp-webrtc-solution/issues/208>
状态：**草稿，未发布**。发布前需确认。

---

## 草稿正文（英文）

**Third independent reproduction on a non-LiveKit endpoint, plus the failing code path**

Adding evidence from a different endpoint stack, and what I believe is the first
code-level identification of the branch that rejects the response.

### Setup

- ESP32-S3 (M5Stack StackChan), ESP-IDF v5.5.4, PSRAM 8 MB
- esp_peer 1.5.6, hand-rolled signaling: one non-trickle offer POSTed, answer applied
- Endpoint: Alibaba Cloud Model Studio Realtime WebRTC (`POST /api/v1/webrtc/realtime`).
  The answer uses `a=setup:passive`, so the device is the **DTLS client**.
  No LiveKit and no TURN are involved — this is an unrelated endpoint stack.

### Symptom: same signature

```
I AGENT: 0 0 Send binding request (cand:0) local0:192.168.1.7:59813 remote0:39.105.73.166:3478 id:5851f42d40b18ccf4bb5f646
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
W AGENT: XOR-MAPPED 124.126.137.141:13174 is not local candidate, skip nominate
... (repeats every ~400 ms until)
I WebRTC-M0: [state] CONNECT_FAILED (8)
```

`124.126.137.141:13174` is the advertised local srflx and equals the mapped address.
Note that, unlike the original report, **the NAT also rewrites the port**
(59813 → 13174), so this is not specific to port-preserving NAT.

### Strict single-variable A/B

Previous version comparisons in this thread did not control other dependencies.
This one changes only esp_peer: both arms use the same M0 code, same ESP-IDF and
sdkconfig, same STUN configuration and network, and the lock file differs only in
the esp_peer `component_hash` and `version` (two lines).

| `esp_peer` | `libpeer_default.a` SHA-256 (first 16) | Result |
|---|---|---|
| 1.5.6 | `6b2f3856b9a1639b` | `skip nominate` → `CONNECT_FAILED` |
| **1.5.5** | `25a338fc6b05f702` | `Select pair` → `Connection OK` → PAIRED → CONNECTED → DATA_CHANNEL_OPENED → `session.created` + `session.updated` |

(Switching versions by deleting `dependencies.lock` re-resolves ~21 unrelated
components and invalidates the comparison; replacing just the esp_peer entry does not.)

### Code path — what is actually new in 1.5.6

Comparing the two published ESP32-S3 static libraries (SHA-256 above, both matching
the official files at the corresponding commits):

1. **`agent_pair_candidate` already filters same-type candidates in 1.5.5.**
   Identical instructions at the same offsets (`0x3f`/`0x41` read the candidate
   `type` at struct offset 16, `0x43` skips the combination when they differ).
   Since 1.5.5 connects here, this pre-existing restriction is not the cause of
   the regression, even though it also prevents local-srflx/remote-host pairing.

2. **`agent_bind_mapped_matches_local` is new in 1.5.6.**
   The symbol is absent from 1.5.5's `agent.c.obj`, and the string
   `is not local candidate, skip nominate` occurs **zero times** in the 1.5.5
   library. Recovered logic (binary-equivalent, not source):

   ```c
   if (agent->mode == AGENT_MODE_CONTROLLING &&
       cur->type != ICE_CANDIDATE_TYPE_RELAY &&
       stun_msg->mapped_addr.port != 0 &&
       !agent_same_addr(&stun_msg->mapped_addr, &cur->local->addr)) {
       /* log, tear down the selected pair's connection state, return */
   }
   ```

   It compares the mapped address only against the **current pair's local
   candidate**, never against the collected candidate list, and does not create a
   prflx or a valid pair from the mapped address
   (RFC 8445 §7.2.5.3.1/2). With a host candidate behind NAT the two can never be
   equal, so no pair is ever nominated. This is the regression.

### One discrepancy worth clarifying

@zigit reported that 1.5.2 also fails, concluding this is not a 1.5.6 regression.
Our controlled test has **1.5.5 succeeding**, which would place the regression
exactly at 1.5.5 → 1.5.6. It would help to know whether that 1.5.2 was the
`third_party` vendored copy rather than the published component, since a tested
1.5.5 is the closest release below 1.5.6.

### Suggested fix direction

Construct or locate the valid pair from the mapped address, while keeping the
required STUN response validation — i.e. do not simply accept any
transaction-matched response.

### Evidence

Full logs, SDPs, the A/B procedure and the disassembly commands:
- https://github.com/MetaAlms/StackChan/blob/feat/aliyun-omni-v2v/docs/firmware-findings.md (A8)
- https://github.com/MetaAlms/StackChan/blob/feat/aliyun-omni-v2v/docs/webrtc-m0-review-result-2.md

**Workaround in use:** pinning `espressif/esp_peer` to exactly `1.5.5`
(not `~1.5.5`, which resolves to 1.5.6).
