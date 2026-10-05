'use strict';

// F04: fresh renderer contexts share only the display store and a backend-owned session, never chat memory.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');
const clone = value => JSON.parse(JSON.stringify(value));
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };

function node() {
 return {
  children: [], isConnected: false, classList: { add() {}, remove() {}, toggle() {} }, style: {},
  append(...items) { for (const item of items) { item.isConnected = true; this.children.push(item); } },
  replaceChildren() { this.children = []; }, remove() { this.isConnected = false; }, before() {},
  querySelector: () => null, querySelectorAll: () => [], getAnimations: () => [], addEventListener() {},
  get childElementCount() { return this.children.length; },
 };
}

function store(messages = [], id = 's1') {
 const data = new Map([
  ['index', { chats: id ? [{ id, folder: '/work', model: 'm1', title: 'Saved chat' }] : [], folders: [] }],
  [`chats/${id}`, { messages: clone(messages), tokens: 0 }],
 ]);
 return { data, read: async key => clone(data.get(key) || null), write: async (key, value) => { data.set(key, clone(value)); }, remove: async key => data.delete(key) };
}

function backend({ exists = true, recovery = true } = {}) {
 const state = { exists, recovery, version: 'incarnation-1', revision: 0, turn: null, calls: [], creates: 0, starts: 0, gets: 0 };
 state.transport = () => {
  return {
   status: async () => ({ state: 'running' }), onStatus() {}, onMessage(fn) { state.deliver = fn; },
   send(request) {
    state.calls.push(clone(request));
    if (!request.id) return;
    (async () => {
     const p = request.params;
     switch (request.method) {
      case 'initialize': return { protocolVersion: '0.1', capabilities: { sessions: { recovery: state.recovery } } };
      case 'session.get': {
       state.gets++;
       const result = state.exists ? { exists: true, sessionVersion: state.version, revision: state.revision,
        turn: state.turn && (p.clientTurnId === state.turn.clientTurnId || state.turn.active) ? clone(state.turn) : null } : { exists: false };
       await state.getGate?.promise;
       return result;
      }
      case 'turn.start': case 'turn.retry': {
       if (state.turn?.clientTurnId === p.clientTurnId) return { turnId: state.turn.turnId, sessionVersion: state.version };
       if (p.sessionVersion === null ? state.exists : !state.exists || p.sessionVersion !== state.version) throw new Error('session_conflict');
       if (state.turn?.active) throw new Error('turn_active');
       if (!state.exists) { state.exists = true; state.creates++; }
       state.starts++;
       state.turn = { clientTurnId: p.clientTurnId, turnId: `t${state.starts}`, input: p.input || null, events: [], active: true };
       await state.startGate?.promise;
       return { turnId: state.turn.turnId, sessionVersion: state.version };
      }
      case 'turn.steer': return { accepted: true };
      default: return null;
     }
    })().then(result => state.deliver({ jsonrpc: '2.0', id: request.id, result }), error => state.deliver({ jsonrpc: '2.0', id: request.id, error: { code: -32000, message: error.message } }));
   },
  };
 };
 state.event = (method, params, { live = true } = {}) => {
  const p = { sessionId: state.sessionId || 's1', turnId: state.turn?.turnId, seq: ++state.revision, ...params };
  if (method === 'turn.completed') state.turn.active = false;
  state.turn?.events.push({ method, params: p });
  if (live) state.deliver({ jsonrpc: '2.0', method, params: p });
  return p;
 };
 return state;
}

async function page(storage, server) {
 const window = vm.createContext({
  console, setTimeout, clearTimeout, AbortController, DOMException, structuredClone, performance,
  navigator: { language: 'en' }, document: { readyState: 'complete', createElement: node },
  openghost: { backend: server.transport(), platform: 'linux' },
  matchMedia: () => ({ matches: true }), ResizeObserver: class { observe() {} unobserve() {} }, RowGlide: class {},
  I18n: { t: key => key }, Usage: { record() {}, parts: () => null },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'library.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 // Replace rendering only; session ownership, load, send, streaming, drive/end and Library persistence are real.
 Object.assign(window.Chat.prototype, {
  followBottom() {}, stopFollow() {}, pin() {}, syncBottom() {},
  assistantMessage() { return { el: node(), stream: { push() {}, finish: async () => {} } }; },
  userMessage: node, entryView: node, toolbar: node, dismissGhost() {}, showGhost() {}, note() {},
  fail(conv, view, error) { conv.failure = error; },
  sessionParams: async () => ({ model: 'm1' }),
 });
 const library = new window.Library(storage, () => {});
 await library.ready;
 const chat = new window.Chat({ main: node(), thread: node(), bottom: node(), library, onChange() {},
  settings: { resolve: id => id || 'm1', configFor: () => ({ id: 'm1', model: 'm1', ready: true }), setModel() {} },
 });
 await window.Backend.ready;
 const open = async (id = 's1') => { await chat.open(id); return chat.active; };
 return { window, chat, library, open };
}

const history = [
 { role: 'user', text: 'old question', content: 'old question' },
 { role: 'assistant', content: 'locally edited diagram', turn: 'old' },
 { role: 'stats', stats: { version: 1 } },
];

test('reload reconciles an existing session, preserving the display cache and sending only new input', async () => {
 const storage = store(history), server = backend();
 const { chat, open } = await page(storage, server);
 const conv = await open();
 assert.equal(conv.reconciled, true);
 assert.equal(conv.sessionVersion, server.version);
 assert.deepEqual(clone(conv.messages), history);
 assert.equal(server.gets, 1);
 assert.equal(chat.send('new question'), true);
 await tick();
 const start = server.calls.find(call => call.method === 'turn.start');
 assert.equal(start.params.sessionVersion, server.version);
 assert.equal(start.params.input.text, 'new question');
 assert.equal('messages' in start.params, false);
 assert.equal(server.creates, 0);
 server.event('turn.completed', { status: 'done' });
 await tick();
});

test('reload after acceptance but before the start reply recovers the active turn and the replay/live race once', async () => {
 const storage = store([], null), server = backend({ exists: false });
 server.startGate = deferred();
 const first = await page(storage, server);
 assert.equal(first.chat.send('in flight'), true);
 await tick();
 const id = first.chat.active.id;
 server.sessionId = id;
 const checkpoint = storage.data.get(`chats/${id}`);
 assert.equal(checkpoint.messages[0].pendingTurn, true);
 assert.equal(checkpoint.messages[0].backendTurn, server.turn.clientTurnId);
 assert.equal(storage.data.get('index').chats[0].id, id);
 server.event('message.started', { messageId: 'm1' }, { live: false });
 const partial = server.event('message.delta', { messageId: 'm1', text: 'partial' }, { live: false });
 first.window.Backend.close({ state: 'exited' });
 await tick();
 const second = await page(storage, server);
 server.getGate = deferred();
 const opening = second.open(id);
 await tick();
 const conv = second.chat.conversations.get(id);
 assert.equal(second.chat.reconcile(conv), conv.recovering, 'concurrent recovery is single-flight');
 // A duplicate of a replayed delta and a new delta race the snapshot response.
 server.deliver({ jsonrpc: '2.0', method: 'message.delta', params: partial });
 server.event('message.delta', { messageId: 'm1', text: ' tail' });
 server.getGate.resolve();
 await opening;
 assert.equal(conv.turn.remote, 't1');
 assert.equal(conv.turn.id, server.turn.clientTurnId);
 assert.equal(conv.messages.filter(entry => entry.role === 'user').length, 1);
 assert.equal(conv.messages.find(entry => entry.role === 'assistant').content, 'partial tail');
 assert.equal(server.starts, 1);
 assert.equal(server.creates, 1);
 assert.equal(server.calls.filter(call => call.method === 'turn.retry').length, 0);
 server.startGate.resolve(); // Old response has the old page's request id and cannot claim a new request.
 server.event('turn.completed', { status: 'done' });
 await tick();
 assert.equal(conv.turn, null);
 assert.equal(conv.messages.some(entry => entry.pendingTurn), false);
 const third = await page(storage, server);
 const restored = await third.open(id);
 assert.equal(restored.messages.find(entry => entry.role === 'assistant').content, 'partial tail');
 assert.equal(server.starts, 1);
 assert.equal(server.creates, 1);
});

test('a turn completed while the page was absent repairs the partial cache without duplicating old text or annotations', async () => {
 const storage = store(history), server = backend();
 const first = await page(storage, server);
 await first.open();
 first.chat.send('last question');
 await tick();
 first.window.Backend.close({ state: 'exited' });
 await tick();
 server.event('message.started', { messageId: 'm1' }, { live: false });
 server.event('message.delta', { messageId: 'm1', text: 'recovered answer' }, { live: false });
 server.event('turn.completed', { status: 'done' }, { live: false });
 const second = await page(storage, server);
 const conv = await second.open();
 await tick();
 assert.deepEqual(clone(conv.messages.slice(0, history.length)), history);
 assert.equal(conv.messages.filter(entry => entry.content === 'last question').length, 1);
 assert.equal(conv.messages.filter(entry => entry.content === 'recovered answer').length, 1);
 assert.equal(conv.turn, null);
 assert.equal(server.starts, 1);
 assert.equal(server.creates, 0);
});

test('visible chat with a missing backend session fails closed, even with an empty cache', async () => {
 for (const messages of [history, []]) {
  const storage = store(messages), server = backend({ exists: false });
  const { chat, open } = await page(storage, server);
  const conv = await open();
  assert.equal(conv.reconciled, false);
  assert.match(conv.recoveryNotice.textContent, /session is missing/);
  assert.deepEqual(clone(conv.messages), messages);
  assert.equal(chat.send('must not create'), false);
  chat.retry(conv, { el: node() });
  await tick();
  assert.equal(server.starts, 0);
  assert.equal(server.creates, 0);
  assert.deepEqual(storage.data.get('chats/s1').messages, messages);
 }
});

test('unsupported recovery and an unaccepted saved turn never become a new turn', async () => {
 for (const recovery of [false, true]) {
  const messages = [...history, { role: 'user', text: 'uncertain', content: 'uncertain', backendTurn: 'lost', pendingTurn: true }];
  const server = backend({ recovery }), storage = store(messages);
  const { chat, open } = await page(storage, server);
  const conv = await open();
  assert.equal(conv.reconciled, false);
  assert.match(conv.recoveryNotice.textContent, recovery ? /did not accept/ : /does not support/);
  assert.equal(chat.send('again'), false);
  await tick();
  assert.equal(server.starts, 0);
  assert.deepEqual(clone(conv.messages), messages);
 }
});

test('a backend losing its session after recovery cannot silently create empty history on Send', async () => {
 const server = backend(), storage = store(history);
 const { chat, open } = await page(storage, server);
 const conv = await open();
 server.exists = false;
 chat.send('new text');
 await tick();
 assert.equal(server.starts, 0);
 assert.equal(server.creates, 0);
 assert.equal(conv.reconciled, false);
 assert.equal(conv.messages.find(entry => entry.pendingTurn).text, 'new text');
 assert.match(conv.failure.message, /session_conflict/);
});

test('a failed durable checkpoint dispatches no turn and the write queue remains usable', async () => {
 const storage = store([], null), server = backend({ exists: false });
 const original = storage.write;
 storage.write = async (key, value) => { if (key.startsWith('chats/')) throw new Error('disk full'); await original(key, value); };
 const { chat } = await page(storage, server);
 chat.send('keep me');
 await tick();
 assert.equal(server.starts, 0);
 assert.match(chat.active.failure.message, /disk full/);
 storage.write = original;
 await chat.checkpoint(chat.active);
 assert.equal(storage.data.get(`chats/${chat.active.id}`).messages[0].text, 'keep me');
});

test('reload preserves a queued input and accepts its late backend event without resending it', async () => {
 const storage = store(history), server = backend();
 const first = await page(storage, server);
 await first.open();
 first.chat.send('start');
 await tick();
 server.event('message.started', { messageId: 'm1' });
 server.event('message.delta', { messageId: 'm1', text: 'before steer' });
 first.chat.send('steered input');
 await tick();
 const sent = server.calls.find(call => call.method === 'turn.steer');
 assert.ok(sent);
 first.window.Backend.close({ state: 'exited' });
 await tick();
 const second = await page(storage, server);
 const conv = await second.open();
 assert.equal(conv.messages.filter(entry => entry.text === 'steered input').length, 1);
 server.event('input.accepted', { clientInputId: sent.params.clientInputId, input: { text: 'steered input', attachments: [] } });
 server.event('message.started', { messageId: 'm2' });
 server.event('message.delta', { messageId: 'm2', text: 'after steer' });
 server.event('turn.completed', { status: 'done' });
 await tick();
 assert.equal(server.calls.filter(call => call.method === 'turn.steer').length, 1);
 assert.equal(conv.messages.filter(entry => entry.text === 'steered input').length, 1);
 assert.deepEqual(clone(conv.messages.filter(entry => entry.role === 'assistant').map(entry => entry.content)), ['locally edited diagram', 'before steer', 'after steer']);
});

test('recovery does not duplicate inputs combined into a start after a quiet operation', async () => {
 const storage = store(history), server = backend();
 const first = await page(storage, server);
 const conv = await first.open();
 const turn = first.chat.begin(conv, first.chat.config(conv)), gate = deferred();
 turn.quiet = true;
 first.chat.openPart(conv, turn);
 first.chat.drive(conv, turn, () => gate.promise);
 first.chat.send('one');
 first.chat.send('two');
 await tick();
 gate.resolve();
 await tick();
 assert.equal(server.turn.input.text, 'one\n\ntwo');
 first.window.Backend.close({ state: 'exited' });
 await tick();
 const second = await page(storage, server);
 const restored = await second.open();
 assert.deepEqual(clone(restored.messages.filter(entry => entry.role === 'user').map(entry => entry.text)), ['old question', 'one', 'two']);
 assert.equal(server.starts, 1);
 server.event('turn.completed', { status: 'done' });
 await tick();
});

test('mini sessions use the same recovery path and retain their separate display checkpoint', async () => {
 const storage = store(history), server = backend();
 const first = await page(storage, server);
 const parent = await first.open();
 const origin = { id: parent.id, record: clone(parent.record) };
 const side = page => new page.window.SideChat({ origin, library: page.library, model: 'm1', settings: page.chat.settings,
  main: node(), thread: node(), bottom: node(), onChange() {},
 });
 server.exists = false;
 server.sessionId = 's1:mini';
 const mini = side(first);
 await mini.start();
 assert.equal(mini.send('side question'), true);
 await tick();
 assert.equal(server.calls.find(call => call.method === 'turn.start').params.sessionId, 's1:mini');
 assert.equal(storage.data.get('mini/s1').messages[0].pendingTurn, true);
 first.window.Backend.close({ state: 'exited' });
 await tick();
 server.event('message.started', { messageId: 'm1' }, { live: false });
 server.event('message.delta', { messageId: 'm1', text: 'side answer' }, { live: false });
 const second = await page(storage, server);
 const restored = side(second);
 await restored.start();
 assert.equal(restored.active.turn.remote, server.turn.turnId);
 assert.equal(restored.active.messages.find(entry => entry.role === 'assistant').content, 'side answer');
 server.event('turn.completed', { status: 'done' });
 await restored.idle();
 await tick();
 assert.deepEqual(storage.data.get('chats/s1').messages, history);
 assert.equal(storage.data.get('mini/s1').messages.some(entry => entry.pendingTurn), false);
 assert.equal(server.starts, 1);
 assert.equal(server.creates, 1);
});

test('renderer reload uses distinct RPC identities and identifies the new connection at initialize', async () => {
 const server = backend(), storage = store();
 const first = await page(storage, server);
 const second = await page(storage, server);
 const init = server.calls.filter(call => call.method === 'initialize');
 assert.notEqual(init[0].id, init[1].id);
 assert.notEqual(init[0].params.connectionId, init[1].params.connectionId);
 first.window.Backend.close({ state: 'exited' });
 second.window.Backend.close({ state: 'exited' });
});
