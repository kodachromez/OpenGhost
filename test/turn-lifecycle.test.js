'use strict';

// Audit F08: a turn's preparation and its start acknowledgement against Stop and turn.completed, through the real RPC
// client and turn lifecycle; rendering/storage are doubles.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, fakeTransport, tick } = require('./helpers');

const deferred = () => {
 let resolve;
 const promise = new Promise(done => { resolve = done; });
 return { promise, resolve };
};

function node() {
 return {
  isConnected: true, classList: { remove() {}, add() {} }, style: {},
  append() {}, before() {}, remove() {}, replaceChildren() {}, addEventListener() {},
  querySelector: () => null, querySelectorAll: () => [], getAnimations: () => [],
 };
}

// `userContext` is what UserContext.ready waits for; the real sessionParams reads it.
async function setup(t, { version = 'V', userContext = Promise.resolve() } = {}) {
 const transport = fakeTransport();
 const window = vm.createContext({
  console, setTimeout, clearTimeout, DOMException, AbortController, navigator: { language: 'en' },
  document: { readyState: 'complete', createElement: node }, openghost: { backend: transport },
  matchMedia: () => ({ matches: true }), I18n: { t: key => key }, Usage: { parts: () => null },
  UserContext: { ready: userContext, forBackend: () => ({ instructions: '' }) },
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
 const saved = [];
 Object.assign(chat, {
  active: conv, nodes: new Map(), config: () => config, settings: { mode: 'ask' },
  library: { chat: () => record, saveMessages() {}, update() {}, cwdOf: () => '/w' }, checkpoint: async () => {},
  assistantMessage: () => ({ el: node(), stream: { push() {}, finish: async () => {} } }),
  userMessage: node, toolbar: node, restore() {}, dismissGhost() {}, showGhost() {}, note() {},
  fail: (_conv, view, error) => { view.error = error; },
  save: () => saved.push(conv.messages.map(entry => ({ ...entry }))),
  onChange() {}, followBottom() {},
 });
 const calls = method => transport.sent.filter(call => call.method === method);
 const reply = (method, result) => transport.deliver({ id: calls(method).at(-1).id, result });
 let seq = 0;
 const event = (method, params = {}) => chat.onEvent(conv, method, { sessionId: 'S', turnId: conv.turn?.remote, seq: ++seq, ...params });
 const run = (attachments = []) => {
  const done = chat.run(conv, { text: 'hi', attachments }, config);
  const turn = conv.turn;
  return { turn, view: turn.part.view, done };
 };
 return { window, transport, chat, conv, calls, reply, event, run, saved };
}

const settles = (promise, ms = 200) => Promise.race([promise.then(() => true, () => true), new Promise(resolve => setTimeout(() => resolve(false), ms))]);

test('Stop while an attachment is still being read ends the turn at once, and it never starts afterwards', async t => {
 const f = await setup(t), file = deferred();
 const { done } = f.run([{ name: 'a.txt', size: 1, ready: file.promise }]);
 await tick();
 assert.equal(f.chat.busy, true);
 f.chat.stop();
 assert.ok(await settles(done), 'Stop waited for the attachment');
 assert.equal(f.chat.busy, false);
 assert.equal(f.conv.reconciled, true);
 assert.equal(f.conv.messages.some(entry => entry.pendingTurn), false);
 file.resolve({ type: 'text', text: 'late' });
 await tick();
 await tick();
 assert.equal(f.calls('turn.start').length, 0);
 assert.equal(f.calls('turn.cancel').length, 0);
});

test('Stop while UserContext is still loading ends the turn at once, and it never starts afterwards', async t => {
 const context = deferred();
 const f = await setup(t, { userContext: context.promise });
 const { done } = f.run();
 await tick();
 assert.equal(f.chat.busy, true);
 f.chat.stop();
 assert.ok(await settles(done), 'Stop waited for UserContext');
 assert.equal(f.chat.busy, false);
 context.resolve();
 await tick();
 await tick();
 assert.equal(f.calls('turn.start').length, 0);
});

test('a backend crash during preparation ends the turn, and a later reconnect does not start it', async t => {
 const file = deferred();
 const f = await setup(t);
 const { turn, done, view } = f.run([{ name: 'a.txt', size: 1, ready: file.promise }]);
 await tick();
 f.transport.setStatus({ state: 'exited', code: 1 });
 // What chat.js's 'closed' listener does for each registered chat (this one is built without its constructor).
 turn.finish({ status: 'error', error: { code: 'backend_crashed', message: f.window.Backend.unavailable().message } });
 assert.ok(await settles(done), 'the crash waited for the attachment');
 assert.equal(view.error?.code, 'backend_crashed');
 f.transport.setStatus({ state: 'running' });
 await tick();
 f.transport.deliver({ id: f.calls('initialize').at(-1).id, result: { protocolVersion: '0.1', capabilities: { sessions: { recovery: true } } } });
 await f.window.Backend.ready;
 file.resolve({ type: 'text', text: 'late' });
 await tick();
 await tick();
 assert.equal(f.calls('turn.start').length, 0);
});

test('turn.completed ends a turn whose start is never answered, and the start fails closed at its deadline', async t => {
 const f = await setup(t);
 f.window.Backend.timeouts.default = 30;
 const { turn, view, done } = f.run();
 await tick();
 const start = f.calls('turn.start')[0];
 f.event('turn.started', { turnId: 'T', clientTurnId: turn.id });
 f.event('turn.completed', { status: 'done' });
 assert.ok(await settles(done), 'turn.completed did not release the unanswered turn.start');
 assert.equal(f.chat.busy, false);
 assert.equal(view.error, undefined);
 // Its incarnation is unconfirmed: the markers stay and Retry is not offered until the start is settled.
 assert.ok(f.conv.messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn));
 assert.equal(view.retryIntent, null);
 await new Promise(resolve => setTimeout(resolve, 60));
 assert.equal(f.window.Backend.pending.size, 0);
 assert.deepEqual(f.transport.last(), { jsonrpc: '2.0', method: '$/cancelRequest', params: { id: start.id } });
 assert.equal(view.error?.code, 'timeout');
 assert.equal(f.conv.reconciled, false);
 assert.equal(f.conv.sessionVersion, 'V');
 assert.ok(f.conv.messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn));
});

test('a start answered after turn.completed released it is still validated and adopted', async t => {
 const f = await setup(t, { version: null });
 const { turn, view, done } = f.run();
 await tick();
 f.event('turn.started', { turnId: 'T', clientTurnId: turn.id });
 f.event('turn.completed', { status: 'done' });
 assert.ok(await settles(done));
 assert.equal(f.chat.busy, false);
 // A Send meanwhile waits for that acknowledgement before it reads the incarnation.
 const next = f.run();
 await tick();
 assert.equal(f.calls('turn.start').length, 1);
 f.reply('turn.start', { turnId: 'T', sessionVersion: 'NEW' });
 await tick();
 assert.equal(f.conv.sessionVersion, 'NEW');
 assert.equal(f.conv.reconciled, true);
 assert.equal(view.error, undefined);
 assert.deepEqual({ ...view.retryIntent }, { failedTurnId: 'T' });
 assert.equal(f.conv.messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn), false);
 assert.equal(f.calls('turn.start')[1].params.sessionVersion, 'NEW');
 f.reply('turn.start', { turnId: 'T2', sessionVersion: 'NEW' });
 await tick();
 f.event('turn.completed', { status: 'done' });
 await next.done;
 assert.equal(next.view.error, undefined);
});

test('an ordinary turn (answered start, then events, then turn.completed) is unchanged', async t => {
 const f = await setup(t);
 const { turn, view, done } = f.run();
 await tick();
 f.reply('turn.start', { turnId: 'T', sessionVersion: 'V' });
 await tick();
 assert.equal(turn.remote, 'T');
 assert.equal(f.chat.busy, true);
 f.event('turn.completed', { status: 'done' });
 await done;
 assert.equal(f.chat.busy, false);
 assert.equal(view.error, undefined);
 assert.equal(f.conv.reconciled, true);
 assert.deepEqual({ ...view.retryIntent }, { failedTurnId: 'T' });
 assert.equal(f.conv.messages.some(entry => entry.pendingTurn), false);
 assert.equal(f.calls('$/cancelRequest').length, 0);
});
