'use strict';

// Exercise start acknowledgements through the real RPC client and turn lifecycle; rendering/storage are doubles.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, fakeTransport, tick } = require('./helpers');

function node() {
 return {
  isConnected: true, classList: { remove() {}, add() {} }, style: {},
  append() {}, before() {}, remove() {}, replaceChildren() {}, addEventListener() {},
  querySelector: () => null, querySelectorAll: () => [], getAnimations: () => [],
 };
}

async function setup(t, version = 'V') {
 const transport = fakeTransport();
 const window = vm.createContext({
  console, setTimeout, clearTimeout, DOMException, AbortController, navigator: { language: 'en' },
  document: { readyState: 'complete', createElement: node }, openghost: { backend: transport },
  matchMedia: () => ({ matches: true }), I18n: { t: key => key }, Usage: { parts: () => null },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 await tick();
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1', capabilities: { sessions: { recovery: true } } } });
 await window.Backend.ready;
 t.after(() => window.Backend.close({ state: 'exited' }));
 const config = { id: 'm', model: 'm', ready: true }, record = { id: 'S' };
 const conv = { id: 'S', record, messages: [], list: node(), reconciled: true, sessionVersion: version, tokens: 0 };
 const chat = Object.create(window.Chat.prototype);
 Object.assign(chat, {
  active: conv, nodes: new Map(), config: () => config,
  library: { chat: () => record, saveMessages() {}, update() {} }, checkpoint: async () => {},
  assistantMessage: () => ({ el: node(), stream: { push() {}, finish: async () => {} } }),
  userMessage: node, toolbar: node, restore() {}, dismissGhost() {}, showGhost() {}, note() {},
  fail: (_conv, view, error) => { view.error = error; },
  onChange() {}, followBottom() {}, sessionParams: async () => ({ model: 'm' }),
 });
 const calls = method => transport.sent.filter(call => call.method === method);
 const reply = (method, result) => {
  const call = calls(method).at(-1);
  assert.ok(call, `expected ${method}`);
  transport.deliver({ id: call.id, result });
 };
 let seq = 0;
 const event = (method, params = {}) => chat.onEvent(conv, method, { sessionId: 'S', turnId: conv.turn?.remote, seq: ++seq, ...params });
 const run = () => {
  const done = chat.run(conv, { text: 'new input', attachments: [] }, config);
  const turn = conv.turn;
  return { turn, view: turn.part.view, done };
 };
 return { chat, conv, calls, reply, event, run };
}

for (const expected of [null, 'V']) for (const early of ['none', 'started', 'completed']) {
 test(`valid ${expected === null ? 'new' : 'existing'}-session start version with early ${early} events`, async t => {
  const f = await setup(t, expected), version = expected ?? 'new/opaque:version';
  const { turn, view, done } = f.run();
  await tick();
  assert.equal(f.calls('turn.start')[0].params.sessionVersion, expected);
  assert.equal(f.calls('turn.start')[0].params.input.text, 'new input');
  if (early !== 'none') f.event('turn.started', { turnId: 'T', clientTurnId: turn.id });
  if (early === 'completed') f.event('turn.completed', { status: 'done' });
  f.reply('turn.start', { turnId: 'T', sessionVersion: version });
  await tick();
  assert.equal(f.conv.sessionVersion, version);
  assert.equal(turn.remote, 'T');
  if (early !== 'completed') f.event('turn.completed', { status: 'done' });
  await done;
  assert.equal(f.conv.reconciled, true);
  assert.equal(f.conv.messages.some(entry => entry.pendingTurn), false);
  assert.equal(view.error, undefined);
  // The following start must require the accepted incarnation, not repeat create-only acceptance.
  const next = f.run();
  await tick();
  assert.equal(f.calls('turn.start')[1].params.sessionVersion, version);
  f.reply('turn.start', { turnId: 'T2', sessionVersion: version });
  await tick();
  f.event('turn.completed', { status: 'done' });
  await next.done;
  assert.equal(next.view.error, undefined);
 });
}

for (const expected of [null, 'V']) for (const version of [undefined, null, '', false, true, 0, 7, {}, ['V']]) {
 test(`start rejects ${version === undefined ? 'missing' : JSON.stringify(version)} sessionVersion for ${expected === null ? 'new' : 'existing'} session`, async t => {
  const f = await setup(t, expected), { turn, view, done } = f.run();
  await tick();
  f.reply('turn.start', { turnId: 'T', ...(version === undefined ? {} : { sessionVersion: version }) });
  await tick();
  assert.match(view.error?.message || '', /session.*version/i);
  await done;
  assert.equal(f.conv.sessionVersion, expected);
  assert.equal(turn.remote, '');
  assert.equal(f.conv.reconciled, false);
  assert.equal(view.retryIntent, null);
  assert.ok(f.conv.messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn));
  f.chat.retry(f.conv, view);
  await tick();
  assert.equal(f.calls('session.get')[0].params.clientTurnId, turn.id);
  f.reply('session.get', { exists: false });
  await tick();
  assert.equal(f.calls('turn.start').length, 1);
  assert.equal(f.calls('turn.retry').length, 0);
 });
}

for (const version of [undefined, 'old-incarnation']) for (const early of [false, true]) {
 test(`${version === undefined ? 'missing' : 'mismatched'} start version fails closed${early ? ' even after early completion' : ''}`, async t => {
  const f = await setup(t), { turn, view, done } = f.run();
  await tick();
  if (early) {
   f.event('turn.started', { turnId: 'T', clientTurnId: turn.id });
   f.event('turn.completed', { status: 'done' });
  }
  f.reply('turn.start', { turnId: 'T', ...(version === undefined ? {} : { sessionVersion: version }) });
  await tick();
  assert.match(view.error?.message || '', /session.*version/i);
  await done;
  assert.equal(f.conv.sessionVersion, 'V');
  assert.equal(turn.remote, early ? 'T' : '');
  assert.equal(f.conv.reconciled, false);
  assert.equal(view.retryIntent, null);
  assert.ok(f.conv.messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn));
 });
}

for (const expected of [null, 'V']) test(`a delayed ${expected === null ? 'create-only' : 'existing-session'} acknowledgement cannot overwrite a changed incarnation`, async t => {
 const f = await setup(t, expected), { turn, view, done } = f.run();
 await tick();
 f.conv.sessionVersion = 'current-incarnation';
 f.reply('turn.start', { turnId: 'OLD', sessionVersion: expected ?? 'old-incarnation' });
 await tick();
 assert.match(view.error?.message || '', /session.*version/i);
 await done;
 assert.equal(f.conv.sessionVersion, 'current-incarnation');
 assert.equal(turn.remote, '');
 assert.equal(f.conv.reconciled, false);
});

test('a resolved start response cannot mutate a replacement turn after create-only reset', async t => {
 const f = await setup(t, null), first = f.run();
 await tick();
 // Resolve the RPC, then replace the local turn before its promise continuation runs.
 f.reply('turn.start', { turnId: 'OLD', sessionVersion: 'old-incarnation' });
 const current = f.chat.begin(f.conv, f.chat.config(f.conv));
 await tick();
 assert.equal(f.conv.sessionVersion, null);
 await first.done;
 assert.equal(f.conv.turn, current);
 assert.equal(current.remote, '');
 assert.equal(first.turn.remote, '');
 assert.equal(f.conv.reconciled, true);
});

test('a resolved start response cannot adopt a version after Stop', async t => {
 const f = await setup(t, null), { turn, done } = f.run();
 await tick();
 f.reply('turn.start', { turnId: 'OLD', sessionVersion: 'old-incarnation' });
 f.chat.abort(f.conv);
 await done;
 assert.equal(f.conv.sessionVersion, null);
 assert.equal(turn.remote, '');
 assert.equal(f.conv.reconciled, false);
});
