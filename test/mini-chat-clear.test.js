'use strict';

// Real MiniChat.wipe -> SideChat.clear -> Library mini-cache deletion; only rendering/transport are doubles.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, fakeTransport, tick } = require('./helpers');
const clone = value => JSON.parse(JSON.stringify(value));

function node() {
 const classes = new Set();
 return {
  children: [], isConnected: false, style: {},
  classList: { add: name => classes.add(name), remove: name => classes.delete(name), contains: name => classes.has(name), toggle() {} },
  append(...items) { for (const item of items) item.isConnected = true; this.children.push(...items); },
  replaceChildren() { this.children = []; }, remove() { this.isConnected = false; }, before() {},
  querySelector: () => null, querySelectorAll: () => [], getAnimations: () => [], addEventListener() {},
  setAttribute() {}, removeAttribute() {}, toggleAttribute() {}, focus() {},
  get childElementCount() { return this.children.length; },
  animate() {
   return this.animation = { finished: Promise.resolve(), cancelled: false, cancel() { this.cancelled = true; } };
  },
 };
}

async function setup(t, reduced = true) {
 const transport = fakeTransport(), errors = [], removed = [];
 const window = vm.createContext({
  console, setTimeout, clearTimeout, AbortController, DOMException, performance,
  navigator: { language: 'en' }, document: { readyState: 'complete', createElement: node },
  openghost: { backend: transport, platform: 'linux' }, alert: message => errors.push(message),
  matchMedia: () => ({ matches: reduced }), ResizeObserver: class { observe() {} unobserve() {} }, RowGlide: class {},
  I18n: { t: key => key }, Usage: { parts: () => null },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'library.js', 'chat.js', 'mini-chat.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 }
 Object.assign(window.Chat.prototype, {
  followBottom() {}, stopFollow() {}, pin() {}, syncBottom() {},
  assistantMessage: () => ({ el: node(), stream: { push() {}, finish: async () => {} } }),
  userMessage: node, entryView: node, toolbar: node, dismissGhost() {}, showGhost() {},
  sessionParams: async () => ({ model: 'm' }),
 });
 await tick();
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1', capabilities: { sessions: { recovery: true, delete: true } } } });
 await window.Backend.ready;
 t.after(() => window.Backend.close({ state: 'exited' }));
 const record = { id: 'parent', folder: '/work', model: 'm', updated: 12 };
 const data = new Map([
  ['index', { chats: [record], folders: [] }],
  ['chats/parent', { messages: [{ role: 'user', text: 'parent history' }], tokens: 80 }],
  ['mini/parent', { messages: [{ role: 'user', text: 'mini question' }, { role: 'assistant', content: 'mini answer' }], tokens: 21, seen: 12 }],
 ]);
 const library = new window.Library({
  read: async key => clone(data.get(key) || null),
  write: async (key, value) => data.set(key, clone(value)),
  remove: async key => { removed.push(key); data.delete(key); },
 }, () => {});
 await library.ready;
 const origin = { id: 'parent', record: library.chat('parent'), turn: { remote: 'parent-turn' } };
 const chat = new window.SideChat({ origin, library, model: 'm', main: node(), thread: node(), bottom: node(), onChange() {},
  settings: { resolve: id => id || 'm', configFor: () => ({ id: 'm', model: 'm', ready: true }) },
 });
 const calls = method => transport.sent.filter(call => call.method === method);
 const reply = (method, result, error) => {
  const call = calls(method).at(-1);
  assert.ok(call, `expected ${method}`);
  transport.deliver({ id: call.id, ...(error ? { error: { code: -32000, message: error } } : { result }) });
 };
 const opening = chat.start();
 await tick();
 reply('session.get', { exists: true, sessionVersion: 'mini-v1', revision: 9, turn: null });
 await opening;
 const conv = chat.active;
 const ui = Object.assign(Object.create(window.MiniChat.prototype), {
  chat, input: { ...node(), value: '' }, field: node(), send: node(), clearButton: node(), text: { text: () => '' },
 });
 const parent = clone(data.get('chats/parent'));
 return { window, chat, conv, ui, library, origin, data, parent, errors, removed, calls, reply };
}

function intact(f, messages, children) {
 assert.equal(f.conv.messages, messages);
 assert.equal(f.conv.list.children, children);
 assert.equal(f.conv.tokens, 21);
 assert.equal(f.conv.sessionVersion, 'mini-v1');
 assert.equal(f.conv.eventSeq, 9);
 assert.equal(f.chat.state.seen, 12);
 assert.equal(f.data.get('mini/parent').messages.length, 2);
 assert.deepEqual(f.removed, []);
 assert.deepEqual(f.data.get('chats/parent'), f.parent);
 assert.ok(f.library.chat('parent'));
 assert.equal(f.origin.turn.remote, 'parent-turn');
}

for (const reduced of [true, false]) {
 test(`mini Clear waits for exact mini-session ACK, deduplicates, and starts a fresh incarnation (reduced motion: ${reduced})`, async t => {
  const f = await setup(t, reduced), messages = f.conv.messages, children = f.conv.list.children;
  const pending = f.ui.wipe();
  await tick();
  const clearing = f.chat.clear();
  assert.equal(f.chat.clear(), clearing, 'concurrent clears share one deletion');
  await f.ui.wipe();
  f.ui.onClear();
  assert.equal(f.ui.armed, undefined, 'cannot rearm while wiping');
  assert.deepEqual(f.calls('session.delete').map(call => call.params.sessionId), ['parent:mini']);
  intact(f, messages, children);
  assert.equal(f.chat.send('must wait'), false);
  f.chat.retry(f.conv, {});
  assert.equal(f.calls('turn.start').length, 0);
  assert.equal(f.calls('turn.retry').length, 0);
  let idle = false;
  const settling = f.chat.idle().then(() => { idle = true; });
  await tick();
  assert.equal(idle, false, 'close/reopen must wait for Clear to settle');
  f.reply('session.delete', null);
  await Promise.all([pending, clearing, settling]);
  assert.deepEqual(f.removed, ['mini/parent']);
  assert.equal(f.conv.messages.length, 0);
  assert.equal(f.conv.list.childElementCount, 0);
  assert.equal(f.conv.tokens, 0);
  assert.equal(f.conv.sessionVersion, null);
  assert.equal(f.conv.reconciled, true);
  assert.equal(f.conv.eventSeq, undefined);
  assert.equal(f.conv.eventTurns.length, 0);
  assert.equal(f.chat.state.seen, 0);
  assert.equal(f.conv.deleting, false);
  assert.deepEqual(f.data.get('chats/parent'), f.parent);
  assert.ok(f.library.chat('parent'));
  assert.equal(f.origin.turn.remote, 'parent-turn');
  assert.deepEqual(f.errors, []);
  if (!reduced) assert.equal(f.conv.list.animation.cancelled, true);
  assert.equal(f.chat.send('fresh question'), true);
  await tick();
  const start = f.calls('turn.start')[0];
  assert.equal(start.params.sessionId, 'parent:mini');
  assert.equal(start.params.sessionVersion, null);
  assert.ok(start.params.clientTurnId);
  f.reply('turn.start', { turnId: 'fresh-turn', sessionVersion: 'mini-v2' });
  await tick();
  f.chat.onEvent(f.conv, 'turn.completed', { sessionId: 'parent:mini', turnId: 'fresh-turn', seq: 1, status: 'done' });
  await f.chat.idle();
  assert.equal(f.conv.turn, null, 'new-incarnation events start at a fresh sequence');
 });

 test(`mini Clear rejection restores the visible chat and can be retried (reduced motion: ${reduced})`, async t => {
  const f = await setup(t, reduced), messages = f.conv.messages, children = f.conv.list.children;
  const pending = f.ui.wipe();
  await tick();
  f.reply('session.delete', null, 'Mini deletion denied');
  await pending;
  intact(f, messages, children);
  assert.deepEqual(f.errors, ['Mini deletion denied']);
  assert.equal(f.conv.deleting, false);
  assert.equal(f.ui.wiping, false);
  if (!reduced) assert.equal(f.conv.list.animation.cancelled, true);
  const retry = f.ui.wipe();
  await tick();
  assert.deepEqual(f.calls('session.delete').map(call => call.params.sessionId), ['parent:mini', 'parent:mini']);
  f.reply('session.delete', null);
  await retry;
  assert.deepEqual(f.removed, ['mini/parent']);
 });
}

for (const reason of ['offline', 'unsupported', 'timeout']) test(`mini Clear ${reason} leaves the mini chat intact and surfaces the error`, async t => {
 const f = await setup(t), messages = f.conv.messages, children = f.conv.list.children;
 if (reason === 'offline') f.window.Backend.close({ state: 'none' });
 if (reason === 'unsupported') f.window.Backend.capabilities.sessions.delete = false;
 if (reason === 'timeout') f.window.Backend.timeouts['session.delete'] = 5;
 await f.ui.wipe();
 intact(f, messages, children);
 assert.equal(f.conv.deleting, false);
 assert.equal(f.ui.wiping, false);
 assert.equal(f.errors.length, 1);
 assert.match(f.errors[0], reason === 'offline' ? /No backend/ : reason === 'unsupported' ? /does not support/ : /did not answer/);
 assert.equal(f.calls('session.delete').length, reason === 'timeout' ? 1 : 0);
});

test('mini Clear starts during its animation so close/reopen cannot bypass the pending deletion', async t => {
 const f = await setup(t, false);
 let finish;
 f.conv.list.animate = () => ({ finished: new Promise(resolve => { finish = resolve; }), cancel() {} });
 const pending = f.ui.wipe();
 assert.equal(f.conv.deleting, true);
 assert.ok(f.chat.clearing);
 await tick();
 assert.deepEqual(f.calls('session.delete').map(call => call.params.sessionId), ['parent:mini']);
 await f.ui.wipe();
 f.reply('session.delete', null);
 await f.chat.idle();
 finish();
 await pending;
 assert.equal(f.calls('session.delete').length, 1);
});

test('mini Clear waits for in-flight recovery and blocks another recovery during deletion', async t => {
 const f = await setup(t);
 const recovering = f.chat.reconcile(f.conv);
 await tick();
 const pending = f.chat.clear();
 assert.equal(f.chat.reconcile(f.conv), recovering);
 assert.equal(f.calls('session.delete').length, 0);
 f.reply('session.get', { exists: true, sessionVersion: 'mini-v1', revision: 10, turn: null });
 await recovering;
 await tick();
 await f.chat.reconcile(f.conv);
 assert.equal(f.calls('session.get').length, 2, 'only open and the already in-flight recovery');
 f.reply('session.delete', null);
 await pending;
 assert.equal(f.conv.eventSeq, undefined);
 assert.equal(f.data.has('mini/parent'), false);
});

test('mini Clear drains its stopped turn before removing the mini cache, without stopping the parent', async t => {
 const f = await setup(t);
 assert.equal(f.chat.send('running mini question'), true);
 await tick();
 f.reply('turn.start', { turnId: 'mini-turn', sessionVersion: 'mini-v1' });
 await tick();
 const pending = f.chat.clear();
 await tick();
 assert.deepEqual(f.calls('turn.cancel').map(call => call.params), [{ sessionId: 'parent:mini', turnId: 'mini-turn' }]);
 f.reply('turn.cancel', null);
 assert.equal(f.conv.turn, null);
 assert.ok(f.data.has('mini/parent'));
 f.reply('session.delete', null);
 await pending;
 await f.chat.idle();
 assert.equal(f.data.has('mini/parent'), false);
 assert.equal(f.conv.reconciled, true, 'stopping the old turn must not leave a cleared session unreconciled');
 assert.equal(f.origin.turn.remote, 'parent-turn');
 assert.deepEqual(f.data.get('chats/parent'), f.parent);
});
