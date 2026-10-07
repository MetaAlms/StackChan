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

const NDC_PATH = process.env.NDC_PATH || '/tmp/wrtc-test/node_modules/node-datachannel';
const ndc = require(NDC_PATH);

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

  const events = { created: false, updated: false, vad: [], asr: [], failed: [], errors: [] };
  let updateSent = false;
  let eventChannel = null;

  function onEvent(json) {
    let m;
    try { m = JSON.parse(json); } catch (_) { return; }
    const t = m.type;
    if (t === 'session.created') {
      events.created = true;
      if (!eventChannel) eventChannel = dcLabel;
      rec(`[cfg] session.created on '${dcLabel}'`);
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
      events.vad.push({ kind: 'started', at: Date.now() });
      rec('[vad] speech_started');
    } else if (t === 'input_audio_buffer.speech_stopped') {
      events.vad.push({ kind: 'stopped', at: Date.now() });
      rec('[vad] speech_stopped');
    } else if (t === 'conversation.item.input_audio_transcription.completed') {
      if (typeof m.transcript === 'string' && typeof m.item_id === 'string' && m.item_id) {
        events.asr.push({ transcript: m.transcript, item_id: m.item_id });
        rec(`[asr] completed item=${m.item_id} "${m.transcript}"`);
      } else {
        rec('[asr] completed without usable transcript/item_id; ignored');
      }
    } else if (t === 'conversation.item.input_audio_transcription.failed') {
      events.failed.push(m.error || {});
      rec(`[asr] FAILED ${JSON.stringify(m.error || {})}`);
    } else if (t === 'error') {
      events.errors.push(m.error || {});
      rec(`[err] ${JSON.stringify(m.error || {})}`);
    }
  }

  let dcLabel = null;
  const dc = pc.createDataChannel('oai-events');
  dcLabel = 'oai-events';
  dc.onOpen(() => rec(`[dc] client channel '${dcLabel}' open`));
  dc.onMessage((msg) => onEvent(typeof msg === 'string' ? msg : msg.toString()));
  // The server also opens its own channel; watch every one.
  pc.onDataChannel((ch) => {
    rec(`[dc] server channel '${ch.getLabel()}' opened`);
    ch.onMessage((msg) => onEvent(typeof msg === 'string' ? msg : msg.toString()));
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
    try { dc.sendMessage(msg); updateSent = true; rec('[cfg] session.update sent'); }
    catch (e) { rec(`[cfg] session.update send failed: ${e.message}`); }
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
  const cfgOk = events.echo && events.echo.type === 'server_vad' &&
                events.echo.threshold === 0.5 && events.echo.silence === 800 &&
                events.echo.asr === 'qwen3-asr-flash-realtime';
  rec(`[cfg] verified: ${cfgOk ? 'YES' : 'NO'}`);
  if (!cfgOk) {
    rec('VERDICT: FAIL - configuration layer (fixture not sent)');
    finish();
    return;
  }

  // ------------------------------------------------------------------ media

  const PT = 111;
  let seq = crypto.randomBytes(2).readUInt16BE(0);
  let ts = crypto.randomBytes(4).readUInt32BE(0) >>> 0;
  const stats = { packets: 0, bytes: 0, pktMin: 1e9, pktMax: 0, firstSeq: seq, firstTs: ts,
                  sendErrors: 0 };
  const clipResults = [];

  function buildRtp(payload) {
    const h = Buffer.alloc(12);
    h[0] = 0x80;                    // V=2, no padding, no extension, CC=0
    h[1] = PT & 0x7f;               // M=0: continuous audio, no silence suppression
    h.writeUInt16BE(seq & 0xffff, 2);
    h.writeUInt32BE(ts >>> 0, 4);
    h.writeUInt32BE(ssrc >>> 0, 8);
    return Buffer.concat([h, payload]);
  }

  async function sendFrames(frames, { pace = true } = {}) {
    const t0 = Date.now();
    let deadline = t0;
    for (const f of frames) {
      const pkt = buildRtp(f);
      // Keep the RTCP SR reporter's clock identical to the RTP header clock.
      // Without this the SR reports a stale timestamp while the headers advance,
      // and the receiver's timeline for this stream is inconsistent.
      config.timestamp = ts >>> 0;
      try { track.sendMessageBinary(pkt); stats.packets++; stats.bytes += pkt.length;
            stats.pktMin = Math.min(stats.pktMin, f.length);
            stats.pktMax = Math.max(stats.pktMax, f.length); }
      catch (e) { stats.sendErrors++; }
      seq = (seq + 1) & 0xffff;
      ts = (ts + 960) >>> 0;
      if (pace) {
        deadline += 20;
        const wait = deadline - Date.now();
        if (wait > 0) await delay(wait);
      }
    }
    return Date.now() - t0;
  }

  // Silence keeps the media timeline continuous while ASR is pending, exactly as
  // the device does; without it the server may never observe end-of-speech.
  const silFrames = [];
  const enc = require('./silence_packets.json');
  for (const s of enc.silence) silFrames.push(Buffer.from(s, 'base64'));

  for (const clip of manifest.clips) {
    const frames = loadFrames(path.join(framesDir, `${clip.id}.opusframes`));
    const before = events.asr.length;
    const r = { id: clip.id, packets: frames.length, mediaMs: clip.media_ms,
                vadStart: 0, vadStop: 0, completed: 0, hits: [], itemId: null,
                transcript: null, waitMs: 0 };

    rec(`[play] ${clip.id} (${frames.length} packets, ${clip.media_ms} ms)`);
    await sendFrames(frames);
    await sendFrames(silFrames.slice(0, 60));   // 1200 ms trailing, as frozen

    const w0 = Date.now();
    while (events.asr.length === before && Date.now() - w0 < 20000) {
      await sendFrames(silFrames);          // keep the cadence during the wait
    }
    r.waitMs = Date.now() - w0;

    const vs = events.vad.filter((v) => v.kind === 'started').length;
    const vt = events.vad.filter((v) => v.kind === 'stopped').length;
    r.vadStart = vs; r.vadStop = vt;
    if (events.asr.length > before) {
      const last = events.asr[events.asr.length - 1];
      r.completed = 1; r.itemId = last.item_id; r.transcript = last.transcript;
      for (const kw of clip.keywords || []) {
        if (last.transcript.includes(kw)) r.hits.push(kw);
      }
    }
    rec(`[clip] ${r.id} vad=${r.vadStart}/${r.vadStop} completed=${r.completed} ` +
        `kw=${r.hits.length} item=${r.itemId} wait=${r.waitMs}ms "${r.transcript || ''}"`);
    clipResults.push(r);

    if (!r.completed || r.hits.length < (clip.keywords || []).length) {
      rec(`[asr] stopping the sequence after ${r.id}: a late result must not be ` +
          `credited to a later clip`);
      break;
    }
  }

  const mediaMs = stats.packets * 20;
  rec(`[media] packets=${stats.packets} bytes=${stats.bytes} pkt=${stats.pktMin}..${stats.pktMax} ` +
      `seq ${stats.firstSeq}..${seq} ts ${stats.firstTs}..${ts} mediaMs=${mediaMs} ` +
      `sendErrors=${stats.sendErrors}`);

  const allOk = clipResults.length === manifest.clips.length &&
    clipResults.every((r) => r.vadStart > 0 && r.vadStop > 0 && r.completed &&
                              r.hits.length === (manifest.clips.find((c) => c.id === r.id).keywords || []).length);
  rec(allOk ? 'VERDICT: PASS - standard stack produced VAD + complete ASR + keywords for all clips'
            : 'VERDICT: FAIL - see per-clip results above');

  finish();

  function finish() {
    fs.writeFileSync(outJson, JSON.stringify({
      config: { nodeDatachannel: require(path.join(NDC_PATH, 'package.json')).version,
                libopus: manifest.libopus, model, ssrc, pt: PT, clock: 48000 },
      cfgOk: !!cfgOk, echo: events.echo, clips: clipResults, stats,
      vadEvents: events.vad.length, asrEvents: events.asr.length,
      failedEvents: events.failed, errorEvents: events.errors,
      verdict: allOk ? 'PASS' : 'FAIL',
    }, null, 2) + '\n');
    rec(`result -> ${outJson}`);
    setTimeout(() => process.exit(0), 500);
  }
}

main().catch((e) => { console.error('FATAL:', e.message); process.exit(1); });
