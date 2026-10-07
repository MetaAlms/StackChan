'use strict';
/**
 * Offline checks for the host control's decision logic (H6-2/H6-3).
 *
 * These cases used to require another cloud session to observe. They are pure
 * functions, so they are checked here instead.
 */
const assert = require('assert');
const L = require('./control_logic');

let pass = 0, fail = 0;
function t(name, fn) {
  try { fn(); console.log(`  ok   ${name}`); pass++; }
  catch (e) { console.log(`  FAIL ${name}: ${e.message}`); fail++; }
}

const clip = (over = {}) => ({
  id: 'zh_x', keywords: ['a', 'b'], hits: ['a', 'b'],
  vadStart: 1, vadStop: 1, sameItemVad: true,
  completed: 1, ownedByClip: true, ...over,
});

console.log('H6-2 ownership');
t('normal: owned completion is picked', () => {
  const ann = L.announcedItems([{ kind: 'started', item_id: 'i1' }], [{ item_id: 'i1' }]);
  const c = L.pickOwnedCompletion([{ item_id: 'i1', transcript: 'x' }], ann);
  assert.strictEqual(c.item_id, 'i1');
});
t('stale/foreign item is not picked', () => {
  const ann = L.announcedItems([{ kind: 'started', item_id: 'i1' }], []);
  assert.strictEqual(L.pickOwnedCompletion([{ item_id: 'OLD', transcript: 'x' }], ann), null);
});
t('a later owned completion wins over an earlier foreign one', () => {
  const ann = L.announcedItems([{ kind: 'started', item_id: 'i2' }], []);
  const c = L.pickOwnedCompletion(
    [{ item_id: 'OLD', transcript: 'x' }, { item_id: 'i2', transcript: 'y' }], ann);
  assert.strictEqual(c.transcript, 'y');
});
t('duplicate of an already-consumed item is not owned', () => {
  const ann = L.announcedItems([{ kind: 'started', item_id: 'i1' }], []);
  assert.strictEqual(L.pickOwnedCompletion([{ item_id: 'i0', transcript: 'z' }], ann), null);
});
t('missing item_id or transcript is not usable', () => {
  const ann = L.announcedItems([{ kind: 'started', item_id: 'i1' }], []);
  assert.strictEqual(L.pickOwnedCompletion([{ item_id: 'i1' }], ann), null);
  assert.strictEqual(L.pickOwnedCompletion([{ transcript: 'x' }], ann), null);
  assert.strictEqual(L.pickOwnedCompletion([{ item_id: '', transcript: 'x' }], ann), null);
});
t('different VAD items are not one valid VAD', () => {
  assert.strictEqual(L.sameItemVad([
    { kind: 'started', item_id: 'i1' }, { kind: 'stopped', item_id: 'i2' }]), false);
  assert.strictEqual(L.sameItemVad([
    { kind: 'started', item_id: 'i1' }, { kind: 'stopped', item_id: 'i1' }]), true);
});

console.log('H6-2 PASS criteria');
t('a fully consistent clip passes', () => assert.strictEqual(L.evaluateClip(clip()).ok, true));
t('missing keywords fails', () =>
  assert.strictEqual(L.evaluateClip(clip({ hits: ['a'] })).ok, false));
t('no VAD stop fails', () =>
  assert.strictEqual(L.evaluateClip(clip({ vadStop: 0 })).ok, false));
t('unowned completion fails', () =>
  assert.strictEqual(L.evaluateClip(clip({ ownedByClip: false })).ok, false));
t('mismatched VAD items fail', () =>
  assert.strictEqual(L.evaluateClip(clip({ sameItemVad: false })).ok, false));
t('failed transcriptions fail', () =>
  assert.strictEqual(L.evaluateClip(clip({ completed: 0 })).ok, false));

console.log('H6-3 run validity');
const run = (over = {}) => ({ sendValid: true, invalidReason: null,
                              clips: [clip()], expectedClips: 1, ...over });
t('clean run passes', () => assert.strictEqual(L.evaluateRun(run()).ok, true));
t('refused/exception/late send fails', () =>
  assert.strictEqual(L.evaluateRun(run({ sendValid: false })).ok, false));
t('invalid input evidence fails', () =>
  assert.strictEqual(L.evaluateRun(run({ invalidReason: 'silence metadata missing' })).ok, false));
t('a short run fails', () =>
  assert.strictEqual(L.evaluateRun(run({ clips: [clip()], expectedClips: 3 })).ok, false));

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
