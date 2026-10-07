'use strict';
/**
 * Pure decision logic for the host media control, extracted so it can be
 * exercised offline (H6-2/H6-3).
 *
 * The runner previously decided everything inline, which meant the only way to
 * test ownership, failure handling or the PASS criteria was another cloud run.
 * These functions have no I/O and no timers, so the cases that matter - stale
 * item, mismatched VAD items, duplicate, failed, missing fields, missing
 * keywords - are checked without a session.
 */

/** Items a clip's own VAD/committed announced. */
function announcedItems(vad, committed) {
  const s = new Set();
  for (const v of vad || []) if (v.item_id) s.add(v.item_id);
  for (const c of committed || []) if (c.item_id) s.add(c.item_id);
  return s;
}

/**
 * Newest completion belonging to this clip, or null.
 *
 * A completion whose item was never announced by this clip must not release the
 * wait: that is how a stale or foreign result used to end the wrong clip.
 */
function pickOwnedCompletion(freshAsr, announced) {
  for (let i = (freshAsr || []).length - 1; i >= 0; i--) {
    const c = freshAsr[i];
    if (!c || typeof c.item_id !== 'string' || !c.item_id) continue;
    if (typeof c.transcript !== 'string') continue;
    if (announced.size === 0 || announced.has(c.item_id)) return c;
  }
  return null;
}

/** True only when some single item has both a start and a stop. */
function sameItemVad(vad) {
  const started = new Set((vad || []).filter((v) => v.kind === 'started' && v.item_id)
                                       .map((v) => v.item_id));
  const stopped = new Set((vad || []).filter((v) => v.kind === 'stopped' && v.item_id)
                                       .map((v) => v.item_id));
  for (const i of started) if (stopped.has(i)) return true;
  return false;
}

/** Per-clip verdict with explicit reasons. */
function evaluateClip(r) {
  const reasons = [];
  if (!(r.vadStart > 0)) reasons.push('no speech_started');
  if (!(r.vadStop > 0)) reasons.push('no speech_stopped');
  if (!r.sameItemVad) reasons.push('start/stop refer to different items');
  if (!r.completed) reasons.push('no owned completed transcription');
  if (!r.ownedByClip) reasons.push('completed item was not announced by this clip');
  if (!(r.hits.length === r.keywords.length)) {
    reasons.push(`keywords ${r.hits.length}/${r.keywords.length}`);
  }
  return { ok: reasons.length === 0, reasons };
}

/** Run verdict. A refused/thrown/late send or a lost input makes it invalid. */
function evaluateRun({ sendValid, invalidReason, clips, expectedClips }) {
  const reasons = [];
  if (invalidReason) reasons.push(`invalid input: ${invalidReason}`);
  if (!sendValid) reasons.push('send path refused, threw, lagged or sent nothing');
  if (clips.length !== expectedClips) {
    reasons.push(`only ${clips.length}/${expectedClips} clips reached a verdict`);
  }
  for (const r of clips) {
    const e = evaluateClip(r);
    if (!e.ok) reasons.push(`${r.id}: ${e.reasons.join('; ')}`);
  }
  return { ok: reasons.length === 0, reasons };
}

module.exports = { announcedItems, pickOwnedCompletion, sameItemVad, evaluateClip, evaluateRun };
