/**
 * Decisive ICE reachability test against Aliyun's WebRTC endpoint.
 * Real standard WebRTC stack (node-datachannel), same network as the device.
 *   datachannel opens -> ICE works; M0 failure is inside esp_peer
 *   never opens       -> endpoint unreachable from here
 * Usage: node test.js <apiKey> <workspaceId> <model>
 */
const ndc = require('node-datachannel');
const https = require('https');

const [apiKey, workspaceId, model] = process.argv.slice(2);
if (!apiKey || !workspaceId || !model) { console.error('usage: node test.js <key> <ws> <model>'); process.exit(2); }

const host = `${workspaceId}.cn-beijing.maas.aliyuncs.com`;
const path = `/api/v1/webrtc/realtime?model=${encodeURIComponent(model)}`;
const t0 = Date.now();
const el = () => `[${String(Date.now() - t0).padStart(6)}ms]`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function exchangeSdp(offer) {
  return new Promise((resolve, reject) => {
    const req = https.request({ host, path, method: 'POST',
      headers: { 'Content-Type': 'application/sdp', 'Authorization': `Bearer ${apiKey}`,
                 'Content-Length': Buffer.byteLength(offer) } }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => {
        console.log(`${el()} HTTP ${res.statusCode}, answer ${body.length} bytes`);
        res.statusCode === 200 ? resolve(body) : reject(new Error(`HTTP ${res.statusCode}: ${body.slice(0,300)}`));
      });
    });
    req.on('error', reject); req.write(offer); req.end();
  });
}

const pc = new ndc.PeerConnection('probe', { iceServers: [] });
let connectionState = 'new', latestSdp = null, gathering = 'new';
pc.onStateChange((s) => { connectionState = s; console.log(`${el()} [pc] ${s}`); });
pc.onIceStateChange((s) => console.log(`${el()} [ice] ${s}`));
pc.onGatheringStateChange((s) => { gathering = s; console.log(`${el()} [gathering] ${s}`); });
pc.onLocalDescription((sdp) => { latestSdp = sdp; console.log(`${el()} local description ready (${sdp.length} bytes)`); });
const localCands = [];
pc.onLocalCandidate((c, mid) => {
  localCands.push(c);
  console.log(`${el()} [candidate] ${mid} ${c}`);
});

// The offer must carry an m=audio section: the endpoint answers HTTP 400
// remote_sdp_failed without it ("不能仅因业务无需播放音频就删除该媒体段").
// Added before the data channel so audio is m-line 0, matching the answer.
try {
  // The track starts with no codecs, which produces "m=audio ... SAVPF " with an
  // empty format list and no a=rtpmap line - the endpoint rejects that as
  // "SDP format and content error". Opus must be registered explicitly.
  // Payload type 111 is what the answer uses.
  const audio = new ndc.Audio('0', 'SendRecv');
  const rtpmap = audio.addOpusCodec(111);
  pc.addTrack(audio);
  console.log(`${el()} added audio track (SendRecv), opus rtpmap: ${rtpmap}`);
} catch (e) {
  console.log(`${el()} addTrack failed: ${e.message}`);
}

const dc = pc.createDataChannel('oai-events');
let dcOpen = false, sawCreated = false, sawUpdated = false, updateSent = false;
function handle(msg, tag) {
  const s = typeof msg === 'string' ? msg : Buffer.from(msg).toString('utf8');
  console.log(`${el()} <<<${tag} ${s.slice(0,300)}`);
  if (s.includes('session.created')) sawCreated = true;
  if (s.includes('session.updated')) sawUpdated = true;
}
const UPDATE = { event_id: 'event_probe', type: 'session.update',
  session: { modalities: ['text','audio'], instructions: '你是一个测试助手，只用一句话回应。',
    turn_detection: { type: 'server_vad', threshold: 0.5, silence_duration_ms: 800 } } };

function sendUpdate(ch, why) {
  try {
    ch.sendMessage(JSON.stringify(UPDATE));
    updateSent = true;
    console.log(`${el()} >>> sent session.update on '${ch.getLabel()}' (${why})`);
  } catch (e) {
    console.log(`${el()} send update failed on '${ch.getLabel()}': ${e.message}`);
  }
}

dc.onOpen(() => {
  dcOpen = true;
  console.log(`${el()} >>> DATA CHANNEL OPEN (oai-events)`);
  sendUpdate(dc, 'client channel opened');
});
dc.onMessage((m) => handle(m, ''));
pc.onDataChannel((ch) => {
  console.log(`${el()} [server-opened channel] ${ch.getLabel()}`);
  ch.onMessage((m) => {
    handle(m, `[${ch.getLabel()}]`);
    // The server pushes session.created on this channel, so reply on it rather
    // than waiting for the client-created channel to open.
    if (!updateSent && String(m).includes('session.created')) {
      sendUpdate(ch, 'replying to session.created');
    }
  });
});

(async () => {
  for (let i = 0; i < 50 && !latestSdp; i++) await sleep(100);
  if (!latestSdp) { console.log(`${el()} VERDICT: FAIL - no local SDP`); process.exit(1); }
  for (let i = 0; i < 100 && gathering !== 'complete'; i++) await sleep(100);
  await sleep(500);

  // localDescription() returns {type, sdp}. Candidates are trickled, so if the
  // SDP does not carry them yet they must be injected: this signalling path has
  // no trickle channel, and a candidate-less offer can never connect.
  const ld = pc.localDescription();
  let offer = (ld && ld.sdp) ? ld.sdp : latestSdp;
  if (!/a=candidate:/.test(offer) && localCands.length) {
    const lines = offer.split('\r\n');
    let idx = lines.findIndex((l) => l.startsWith('a=mid:'));
    if (idx < 0) idx = lines.findIndex((l) => l.startsWith('m='));
    lines.splice(idx + 1, 0, ...localCands.map((c) => (c.startsWith('a=') ? c : 'a=' + c)));
    offer = lines.join('\r\n');
    console.log(`${el()} injected ${localCands.length} candidate(s) into the offer`);
  }
  const cands = offer.match(/a=candidate:.*/g) || [];
  console.log(`${el()} offer ${offer.length} bytes, ${cands.length} candidate line(s)`);
  cands.forEach((c) => console.log(`      ${c}`));

  console.log('----- OFFER SDP -----');
  console.log(offer.replace(/\r\n/g, '\n'));
  console.log('---------------------');

  let answer;
  try { answer = await exchangeSdp(offer); }
  catch (e) { console.log(`${el()} VERDICT: FAIL - exchange rejected: ${e.message}`); process.exit(1); }

  const rc = answer.match(/a=candidate:.*/g) || [];
  console.log(`${el()} remote candidates (${rc.length}):`);
  rc.forEach((c) => console.log(`      ${c}`));
  console.log(`${el()} remote setup=${(answer.match(/a=setup:(\S+)/) || [])[1]}`);

  try { pc.setRemoteDescription(answer, 'answer'); console.log(`${el()} answer applied`); }
  catch (e) { console.log(`${el()} setRemoteDescription threw: ${e.message}`); }

  for (let i = 0; i < 120 && !dcOpen; i++) await sleep(250);

  console.log('\n==================== PROBE VERDICT ====================');
  console.log(`  peer connection state : ${connectionState}`);
  console.log(`  ice state             : ${pc.iceState()}`);
  console.log(`  data channel open     : ${dcOpen ? 'YES' : 'no'}`);
  console.log(`  session.created       : ${sawCreated ? 'YES' : 'no'}`);
  console.log(`  session.updated       : ${sawUpdated ? 'YES' : 'no'}`);
  console.log(dcOpen ? '  => ICE WORKS here. The M0 failure is inside esp_peer.'
                     : '  => ICE FAILS with a standard stack too; endpoint unreachable from here.');
  console.log('======================================================');
  process.exit(0);
})();
