#!/usr/bin/env node
/**
 * R4-1: standard-stack media positive control.
 *
 * Same frozen fixture, same endpoint/model, same server_vad/ASR configuration
 * as the device path, but a standard WebRTC stack (node-datachannel 0.33.4 ->
 * libdatachannel v0.24.5) with host libopus. This discriminates
 * "server/model/config cannot handle this audio" from "the device's own media
 * path is at fault". A pass here proves only that this standard stack works; it
 * does not prove the device emits the same bytes.
 *
 * Media shape (per docs/webrtc-media-m1-standard-control.md):
 *   - `RtpPacketizationConfig` + `RtcpSrReporter` + `RtcpNackResponder`, and
 *     deliberately NO RtpPacketizer, so the application supplies a complete
 *     12-byte RTP header followed by exactly one raw Opus packet. The generic
 *     packetizer would also set M=1 on every frame, which RFC 7587/3551 forbid
 *     for continuous audio, so building the header here removes that variable.
 *   - timestamp advances by exactly 960 per 20 ms packet; sequence by 1.
 *
 * Usage:
 *   node standard_media_control.js <framesDir> <outJson>
 * Credentials come from the gitignored firmware sdkconfig; nothing sensitive
 * is printed (no API key, no Authorization, no full SDP, no ICE credentials).
 */

'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const https = require('https');
const { setTimeout: delay } = require('node:timers/promises');
// H6-3: Date.now is wall time and can step; performance.now is monotonic, which
// is what an absolute media deadline must be measured against.
const { performance } = require('node:perf_hooks');
const now = () => performance.now();

const NDC_PATH = process.env.NDC_PATH || '/tmp/wrtc-test/node_modules/node-datachannel';
const ndc = require(NDC_PATH);
const L = require('./control_logic');

// ---------------------------------------------------------------- credentials

function readSdkconfig() {
  const p = path.resolve(__dirname, '../../firmware/sdkconfig');
  const txt = fs.readFileSync(p, 'utf8');
  const get = (k) => {
    const m = txt.match(new RegExp(`^${k}=(.*)$`, 'm'));
    return m ? m[1].trim().replace(/^"|"$/g, '') : null;
  };
  const key = get('CONFIG_STACKCHAN_ALIYUN_API_KEY');
  const ws = get('CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID');
  const model = get('CONFIG_STACKCHAN_ALIYUN_MODEL');
  if (!key || !ws || !model) throw new Error('missing Aliyun config in sdkconfig');
  return { key, ws, model };
}

// ------------------------------------------------------------------- framing

function loadFrames(file) {
  const buf = fs.readFileSync(file);
  const out = [];
  let off = 0;
  while (off + 4 <= buf.length) {
    const n = buf.readUInt32BE(off);
    off += 4;
    if (off + n > buf.length) throw new Error(`${file}: truncated frame`);
    out.push(buf.subarray(off, off + n));
    off += n;
  }
  return out;
}

// ------------------------------------------------------------------- run log

const log = [];
const rec = (s) => { console.log(s); log.push(s); };

async function main() {
  const framesDir = process.argv[2] || '/tmp/host_media';
  const outJson = process.argv[3] || '/tmp/host_media/result.json';
  const { key, ws, model } = readSdkconfig();
  const manifest = JSON.parse(fs.readFileSync(path.join(framesDir, 'manifest.json'), 'utf8'));

  ndc.initLogger('Error', () => {});
  rec(`node-datachannel ${require(path.join(NDC_PATH, 'package.json')).version}`);
  try { rec(`library version: ${ndc.getLibraryVersion()}`); } catch (_) {}

  const ssrc = (() => { let v = 0; while (!v) v = crypto.randomBytes(4).readUInt32BE(0); return v; })();
  const cname = 'm1-standard-control';

  const audio = new ndc.Audio('0', 'SendRecv');
  audio.addOpusCodec(111);
  audio.addSSRC(ssrc, cname, 'm1-probe', 'audio0');

  // node-datachannel's constructor takes a name and a config object.
  const pc = new ndc.PeerConnection('m1-control', { iceServers: [] });
  const track = pc.addTrack(audio);   // keep the Track: addTrack alone only edits SDP
  const config = new ndc.RtpPacketizationConfig(ssrc, cname, 111, 48000);
  const sr = new ndc.RtcpSrReporter(config);
  sr.addToChain(new ndc.RtcpNackResponder());
  track.setMediaHandler(sr);          // full RTP route, no packetizer

  pc.onLocalDescription((sdp) => { currentSdp = sdp; });
  pc.onLocalCandidate((c) => { candidates.push(c); });
  let currentSdp = null;
  const candidates = [];

  // H3: all state is initialised before any branch can reach finish(). Declaring
  // consts further down meant a configuration failure hit the temporal dead zone
  // and lost the very evidence the failure needed.
  const events = { created: false, updated: false, vad: [], committed: [], asr: [],
                   failed: [], errors: [], unknown: [], echo: null };
  let updateSent = false;
  let updateOk = false;
  let eventChannel = null;        // the actual channel object, not a label
  let eventChannelLabel = null;
  let cfgOk = false;
  let allOk = false;
  const PT = 111;
  let seq = 0, ts = 0;
  const stats = { accepted: 0, refused: 0, exceptions: 0, bytes: 0, pktMin: 1e9, pktMax: 0,
                  firstSeq: 0, firstTs: 0, lateFrames: 0 };
  const clipResults = [];
  let mediaWallMs = 0;
  let mediaFirstTs = 0, mediaLastTs = 0;
  const silFrames = [];
  // H6-3: these are read by finish() on every path, so they must exist before
  // any branch can reach it. Declaring `extraSilenceFrames` later made the
  // configuration-failure path throw a ReferenceError and lose its evidence.
  let extraSilenceFrames = 0;
  let inputEvidence = null;
  let invalidReason = null;
  let stage = 'start';

  function onEvent(json, ch, label) {
    let m;
    try { m = JSON.parse(json); } catch (_) { return; }
    const t = m.type;
    if (t === 'session.created') {
      events.created = true;
      // H3: reply on the channel this event actually arrived on.
      if (!eventChannel && ch) {
        eventChannel = ch;
        eventChannelLabel = label;
      }
      rec(`[cfg] session.created on '${label}' (replying there)`);
      sendUpdate();
    } else if (t === 'session.updated') {
      const s = m.session || {};
      const td = s.turn_detection || {};
      const tr = s.input_audio_transcription || {};
      events.updated = true;
      events.echo = { type: td.type, threshold: td.threshold,
                      silence: td.silence_duration_ms, asr: tr.model };
      rec(`[cfg] session.updated echo: ${JSON.stringify(events.echo)}`);
    } else if (t === 'input_audio_buffer.speech_started') {
      // H2: keep the item the VAD actually refers to, not just a timestamp.
      events.vad.push({ kind: 'started', at: Date.now(), item_id: m.item_id || null,
                        audio_start_ms: m.audio_start_ms });
      rec(`[vad] speech_started item=${m.item_id || '-'}`);
    } else if (t === 'input_audio_buffer.speech_stopped') {
      events.vad.push({ kind: 'stopped', at: Date.now(), item_id: m.item_id || null,
                        audio_end_ms: m.audio_end_ms });
      rec(`[vad] speech_stopped item=${m.item_id || '-'}`);
    } else if (t === 'input_audio_buffer.committed') {
      events.committed.push({ at: now(), item_id: m.item_id || null });
      rec(`[vad] committed item=${m.item_id || '-'}`);
    } else if (t === 'conversation.item.input_audio_transcription.completed') {
      if (typeof m.transcript === 'string' && typeof m.item_id === 'string' && m.item_id) {
        events.asr.push({ transcript: m.transcript, item_id: m.item_id });
        rec(`[asr] completed item=${m.item_id} "${m.transcript}"`);
      } else {
        rec('[asr] completed without usable transcript/item_id; ignored');
      }
    } else if (t === 'conversation.item.input_audio_transcription.failed') {
      // H7-1: keep the item so a failure can be attributed to a clip.
      events.failed.push(Object.assign({}, m.error || {}, { item_id: m.item_id || null }));
      rec(`[asr] FAILED ${JSON.stringify(m.error || {})}`);
    } else if (t === 'error') {
      events.errors.push(m.error || {});
      rec(`[err] ${JSON.stringify(m.error || {})}`);
    } else {
      // H4: record unknown types by name so a real event is not silently missed.
      events.unknown.push(t);
      rec(`[event] ${t}`);
    }
  }

  const dcLabel = 'oai-events';
  const dc = pc.createDataChannel(dcLabel);
  dc.onOpen(() => rec(`[dc] client channel '${dcLabel}' open`));
  dc.onMessage((msg) => onEvent(typeof msg === 'string' ? msg : msg.toString(), dc, dcLabel));
  // The server also opens its own channel; every event is attributed to the
  // channel object it actually arrived on.
  pc.onDataChannel((ch) => {
    const label = ch.getLabel();
    rec(`[dc] server channel '${label}' opened`);
    ch.onMessage((msg) => onEvent(typeof msg === 'string' ? msg : msg.toString(), ch, label));
  });

  function sendUpdate() {
    if (updateSent) return;
    const msg = JSON.stringify({
      event_id: 'event_host_cfg', type: 'session.update',
      session: {
        modalities: ['text', 'audio'],
        turn_detection: { type: 'server_vad', threshold: 0.5, silence_duration_ms: 800 },
        input_audio_transcription: { model: 'qwen3-asr-flash-realtime' },
      },
    });
    const target = eventChannel || dc;
    try {
      const ok = target.sendMessage(msg);
      updateSent = true;
      updateOk = (ok !== false);
      rec(`[cfg] session.update sent on '${eventChannelLabel || dcLabel}' accepted=${updateOk}`);
    } catch (e) {
      rec(`[cfg] session.update send threw: ${e.message}`);
    }
  }

  // -------------------------------------------------------------- signaling

  pc.onStateChange((s) => rec(`[pc] state ${s}`));

  // Non-trickle: wait for gathering to settle, then post the offer.
  await delay(1500);
  const offer = pc.localDescription().sdp;
  rec(`[sdp] offer ${offer.length} bytes`);

  const host = `${ws}.cn-beijing.maas.aliyuncs.com`;
  const answer = await new Promise((resolve, reject) => {
    const req = https.request({
      host, path: `/api/v1/webrtc/realtime?model=${encodeURIComponent(model)}`,
      method: 'POST',
      headers: { 'Content-Type': 'application/sdp', Authorization: `Bearer ${key}`,
                 'Content-Length': Buffer.byteLength(offer) },
    }, (res) => {
      let body = '';
      res.on('data', (d) => { body += d; });
      res.on('end', () => res.statusCode === 200
        ? resolve(body) : reject(new Error(`HTTP ${res.statusCode}: ${body.slice(0, 200)}`)));
    });
    req.on('error', reject);
    req.write(offer);
    req.end();
  });
  rec(`[sdp] answer ${answer.length} bytes`);
  const mLine = (answer.match(/^m=audio.*$/m) || ['(none)'])[0];
  const rtpmap = (answer.match(/^a=rtpmap:111.*$/m) || ['(none)'])[0];
  const dir = (answer.match(/^a=(sendrecv|recvonly|sendonly|inactive).*$/m) || ['(none)'])[0];
  rec(`[sdp] answer ${mLine} | ${rtpmap} | ${dir}`);

  pc.setRemoteDescription(answer, 'answer');

  // Wait for the config echo.
  const t0 = Date.now();
  while (!events.updated && Date.now() - t0 < 25000) await delay(200);
  // cfgOk is declared up front (H3); assign here rather than redeclaring.
  cfgOk = !!(events.echo && events.echo.type === 'server_vad' &&
             events.echo.threshold === 0.5 && events.echo.silence === 800 &&
             events.echo.asr === 'qwen3-asr-flash-realtime');
  rec(`[cfg] verified: ${cfgOk ? 'YES' : 'NO'}`);
  if (!cfgOk) {
    rec('VERDICT: FAIL - configuration layer (fixture not sent)');
    finish();
    return;
  }

  // ------------------------------------------------------------------ media

  // H6-1: the silence must come from the same generated directory as the voice
  // frames. Reading a stale copy from the script directory meant the run used
  // different audio than the manifest described, so the manifest's own numbers
  // could not evidence it.
  const silPath = path.join(framesDir, 'silence_packets.json');
  const manPath = path.join(framesDir, 'manifest.json');
  const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');
  const silRaw = fs.readFileSync(silPath);
  const manRaw = fs.readFileSync(manPath);
  const sil = JSON.parse(silRaw.toString('utf8'));

  if (typeof sil.decoded_samples !== 'number' || sil.decoded_samples !== 960 ||
      !Array.isArray(sil.silence) || sil.silence.length !== sil.frames ||
      typeof sil.peak_abs !== 'number') {
    invalidReason = 'silence file is missing its validation metadata';
  }
  for (const s of sil.silence) silFrames.push(Buffer.from(s, 'base64'));
  rec(`[sil] ${sil.frames} frames (${sil.ms} ms), mode=${sil.mode}, ` +
      `decoded ${sil.decoded_samples} samples/frame, peak |x|=${sil.peak_abs}, ` +
      `pkt ${sil.pkt_min}..${sil.pkt_max}, sha256 ${sha(silRaw).slice(0, 16)}…`);
  // H6-1: record the hashes of the inputs this run actually consumed, so the
  // archived result identifies its own evidence instead of relying on a
  // separately produced manifest.
  inputEvidence = {
    framesDir,
    manifestSha256: sha(manRaw),
    silenceSha256: sha(silRaw),
    silence: { frames: sil.frames, ms: sil.ms, mode: sil.mode,
               decoded_samples: sil.decoded_samples, peak_abs: sil.peak_abs,
               pkt_min: sil.pkt_min, pkt_max: sil.pkt_max },
    generator: sil.generator,
  };
  if (invalidReason) { rec(`INVALID: ${invalidReason}`); allOk = false; finish(); return; }

  // H1: one monotonic absolute deadline for the whole media run, shared by every
  // clip, silence stretch and ASR wait. Re-deriving it per segment hid boundary
  // cost and drift, and a nominal timestamp increment is not pacing evidence.
  let deadline = now();
  const pacingStart = now();

  function buildRtp(payload) {
    const h = Buffer.alloc(12);
    h[0] = 0x80;                    // V=2, no padding, no extension, CC=0
    h[1] = PT & 0x7f;               // M=0: continuous audio
    h.writeUInt16BE(seq & 0xffff, 2);
    h.writeUInt32BE(ts >>> 0, 4);
    h.writeUInt32BE(ssrc >>> 0, 8);
    return Buffer.concat([h, payload]);
  }

  function sendOne(payload) {
    const pkt = buildRtp(payload);
    // H1: the wrapper returns the Track's real acceptance result. A false return
    // (direction mismatch, keys not ready) does not throw, so ignoring it counted
    // refused frames as sent.
    let ok = false;
    let threw = false;
    try { ok = track.sendMessageBinary(pkt) !== false; }
    catch (e) { threw = true; }
    // Exactly one of the three counters moves: a frame that threw is not also a
    // refusal, and only a true result is an accepted frame.
    if (threw) { stats.exceptions++; }
    else if (ok === true) { stats.accepted++; stats.bytes += pkt.length;
                            stats.pktMin = Math.min(stats.pktMin, payload.length);
                            stats.pktMax = Math.max(stats.pktMax, payload.length); }
    else { stats.refused++; }
    if (stats.accepted === 0 && stats.refused === 0) { stats.firstSeq = seq; stats.firstTs = ts; }
    seq = (seq + 1) & 0xffff;
    ts = (ts + 960) >>> 0;
  }

  async function sendFrames(frames) {
    for (const f of frames) {
      sendOne(f);
      deadline += 20;
      const wait = deadline - now();
      if (wait > 0) await delay(wait);
      else if (wait < -20) {
        // Falling this far behind means a burst would be sent to catch up, which
        // is not the continuous pacing the experiment claims to test.
        stats.lateFrames++;
      }
    }
  }

  if (typeof track.isOpen === 'function') rec(`[track] isOpen=${track.isOpen()}`);
  stats.firstSeq = seq; stats.firstTs = ts;
  mediaFirstTs = ts;

  for (const clip of manifest.clips) {
    // H2: the runner must not score a clip without acceptance data.
    if (!Array.isArray(clip.keywords) || clip.keywords.length === 0 ||
        typeof clip.text !== 'string' || !clip.text) {
      rec(`FATAL: ${clip.id} has no frozen text/keywords; refusing to score 0/0 as a pass`);
      allOk = false;
      finish();
      return;
    }
    const framesPath = path.join(framesDir, `${clip.id}.opusframes`);
    const framesRaw = fs.readFileSync(framesPath);
    const frames = loadFrames(framesPath);
    // H6-1: the voice container must be internally consistent and match the
    // manifest before anything is sent.
    if (typeof clip.tail_pad_samples === 'number') {
      // recorded by the generator; kept for the evidence record
    }
    if (frames.length !== clip.packets) {
      invalidReason = `${clip.id}: container has ${frames.length} packets, ` +
                      `manifest says ${clip.packets}`;
    }
    if (!clip.decoded_samples_per_packet || clip.decoded_samples_per_packet !== 960) {
      invalidReason = `${clip.id}: manifest lacks per-packet decode evidence`;
    }
    if (!clip.ctl || clip.ctl.bitrate !== 0 || clip.ctl.bitrate_readback !== 90000) {
      invalidReason = `${clip.id}: encoder CTL readback missing or unexpected`;
    }
    if (invalidReason) { rec(`INVALID: ${invalidReason}`); allOk = false; finish(); return; }
    const r = { id: clip.id, text: clip.text, keywords: clip.keywords,
                containerSha256: sha(framesRaw),
                packets: frames.length, mediaMs: clip.media_ms,
                vadStart: 0, vadStop: 0, itemIds: [], completed: 0, hits: [],
                itemId: null, transcript: null, waitMs: 0, window: null };

    const clipStart = now();
    const clipStartTs = ts;
    const asrBefore = events.asr.length;
    const vadBefore = events.vad.length;
    const committedBefore = events.committed.length;
    const failedBefore = events.failed.length;

    rec(`[play] ${clip.id} "${clip.text}" (${frames.length} packets, ${clip.media_ms} ms)`);
    await sendFrames(frames);
    await sendFrames(silFrames);          // frozen 1200 ms trailing silence
    const clipEndTs = ts;

    // Keep the cadence while ASR is pending and give the server time to close
    // the turn; the media timeline stays continuous throughout.
    // The items this clip's turn actually announced. A completion is only
    // allowed to end this clip when it carries one of them; a stale or unknown
    // item must not release the wait.
    // H7-1: the set is re-derived on every poll. Freezing it before the wait
    // meant an item announced only during the wait could never release it.
    const currentAnnounced = () => L.announcedItems(events.vad.slice(vadBefore),
                                                    events.committed.slice(committedBefore));

    const w0 = now();
    let owned = null;
    let announced = currentAnnounced();
    while (now() - w0 < 20000) {
      announced = currentAnnounced();
      owned = L.pickOwnedCompletion(events.asr.slice(asrBefore), announced);
      if (owned) break;
      if (events.failed.slice(failedBefore).some((f) => !f.item_id || announced.size === 0 ||
                                                  announced.has(f.item_id))) break;
      await sendFrames(silFrames);
      extraSilenceFrames += silFrames.length;
    }
    r.waitMs = Math.round(now() - w0);
    // 48 RTP ticks per millisecond at the 48 kHz Opus clock.
    r.window = { startTs: clipStartTs, endTs: clipEndTs,
                 startMs: Math.round(clipStartTs / 48), endMs: Math.round(clipEndTs / 48),
                 rtpTicksPerMs: 48 };

    // H2: attribute VAD and ASR to *this* clip only, by arrival window.
    const myVad = events.vad.slice(vadBefore);
    r.vadStart = myVad.filter((v) => v.kind === 'started').length;
    r.vadStop = myVad.filter((v) => v.kind === 'stopped').length;
    r.itemIds = [...new Set(myVad.map((v) => v.item_id).filter(Boolean))];

    r.committedItems = [...new Set(events.committed.slice(committedBefore)
                                     .map((c) => c.item_id).filter(Boolean))];
    if (owned) {
      r.completed = 1; r.itemId = owned.item_id; r.transcript = owned.transcript;
      for (const kw of clip.keywords) {
        if (owned.transcript.includes(kw)) r.hits.push(kw);
      }
    }
    // Ownership is a criterion, not a warning: the completing item must be one
    // this clip's own VAD/committed announced, and its start/stop must refer to
    // that same item rather than to two different ones.
    r.sameItemVad = L.sameItemVad(myVad);
    r.bracketedItem = L.bracketedItem(myVad);
    r.ownedByClip = !!(r.itemId && announced.has(r.itemId));
    if (r.completed && !r.ownedByClip) {
      rec(`[asr] ${r.id}: completed item=${r.itemId} was NOT announced by this clip ` +
          `(${[...announced].join(',') || '-'}); not credited`);
      r.completed = 0; r.itemId = null; r.transcript = null; r.hits = [];
    }
    if (r.vadStart > 0 && r.vadStop > 0 && !r.sameItemVad) {
      rec(`[asr] ${r.id}: start/stop refer to different items; VAD not accepted`);
    }
    rec(`[clip] ${r.id} vad=${r.vadStart}/${r.vadStop} sameItemVad=${r.sameItemVad} ` +
        `announced=${r.itemIds.concat(r.committedItems).join('|') || '-'} ` +
        `completed=${r.completed} owned=${r.ownedByClip} ` +
        `kw=${r.hits.length}/${clip.keywords.length} item=${r.itemId || '-'} ` +
        `wait=${r.waitMs}ms "${r.transcript || ''}"`);
    clipResults.push(r);

    // H2: stop the sequence on a miss or an outright failure; a late result must
    // not be credited to a later clip, and a failed turn is a real miss.
    if (!r.completed || r.hits.length < clip.keywords.length ||
        r.vadStart === 0 || r.vadStop === 0 || !r.sameItemVad || !r.ownedByClip ||
        events.failed.length > failedBefore) {
      rec(`[asr] stopping the sequence after ${r.id}`);
      break;
    }
  }

  mediaWallMs = Math.round(now() - pacingStart);
  mediaLastTs = ts;
  const mediaMs = stats.accepted * 20;
  rec(`[media] accepted=${stats.accepted} refused=${stats.refused} exceptions=${stats.exceptions} ` +
      `bytes=${stats.bytes} pkt=${stats.pktMin === 1e9 ? 0 : stats.pktMin}..${stats.pktMax} ` +
      `seq ${stats.firstSeq}..${seq} ts ${stats.firstTs}..${ts} lateFrames=${stats.lateFrames}`);
  rec(`[time] mediaMs(${mediaMs}, from accepted frames) vs wallMs(${mediaWallMs}) over the ` +
      `same run; extra silence frames=${extraSilenceFrames}`);

  // H1: a refused or throwing send makes the run invalid; it is reported as such
  // and never confused with "the remote received it".
  const sendValid = (stats.refused === 0 && stats.exceptions === 0 &&
                     stats.accepted > 0 && stats.lateFrames === 0 &&
                     invalidReason === null);
  if (!sendValid) rec('INVALID: the send path refused or threw; results cannot be attributed');

  // The same evaluator the offline tests exercise decides the run.
  const verdict = L.evaluateRun({ sendValid, invalidReason,
                                  clips: clipResults, expectedClips: manifest.clips.length });
  allOk = verdict.ok;
  if (!allOk) rec(`[verdict] reasons: ${verdict.reasons.join(' | ')}`);
  rec(allOk
    ? 'VERDICT: PASS - standard stack produced VAD start+stop, complete ASR and keywords for all three clips'
    : 'VERDICT: FAIL - see per-clip results above');

  finish();

  function finish() {
    const out = {
      config: { nodeDatachannel: require(path.join(NDC_PATH, 'package.json')).version,
                libopus: manifest.libopus, model, ssrc, pt: PT, clock: 48000 },
      cfgOk, updateOk, eventChannelLabel, stage, invalidReason, inputEvidence,
      echo: events.echo, clips: clipResults, stats,
      mediaWallMs, mediaFirstTs, mediaLastTs, extraSilenceFrames,
      vadEvents: events.vad.length, asrEvents: events.asr.length,
      failedEvents: events.failed, errorEvents: events.errors, unknownEvents: events.unknown,
      sendValid: (stats.refused === 0 && stats.exceptions === 0 && stats.accepted > 0),
      verdict: allOk ? 'PASS' : 'FAIL',
    };
    fs.writeFileSync(outJson, JSON.stringify(out, null, 2) + '\n');
    // H3: release the session's resources and make the exit code match the verdict.
    try { track.stop(); } catch (_) {}
    try { pc.close(); } catch (_) {}
    try { ndc.cleanup(); } catch (_) {}
    rec(`result -> ${outJson}`);
    setTimeout(() => process.exit(allOk ? 0 : 2), 300);
  }
}

main().catch((e) => { console.error('FATAL:', e.message); process.exit(1); });
