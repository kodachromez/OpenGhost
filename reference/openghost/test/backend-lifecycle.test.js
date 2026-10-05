'use strict';

// F20: exercise the real preload subscriptions and renderer diagnostics, without launching Electron/a backend.
const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');
const source = file => fs.readFileSync(path.join(ROOT, file), 'utf8');
const node = () => ({ addEventListener() {}, querySelectorAll: () => [], querySelector: () => null });

function page(t, status = { state: 'running' }, document = { readyState: 'complete' }) {
 const ipc = new EventEmitter(), sent = [];
 // Backend messages go by invoke, which answers whether the host delivered them.
 ipc.invoke = async (channel, message) => {
  if (channel === 'backend:send') { sent.push(message); return true; }
  assert.equal(channel, 'backend:status');
  return status;
 };
 let bridge;
 vm.runInNewContext(source('desktop/preload.js'), {
  process: { platform: 'linux' },
  require: name => {
   assert.equal(name, 'electron');
   return { ipcRenderer: ipc, contextBridge: { exposeInMainWorld: (name, value) => { bridge = value; } } };
  },
 });
 const window = vm.createContext({
  console, setTimeout, clearTimeout, AbortController, DOMException, document,
  navigator: { language: 'en' }, openghost: bridge,
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  ResizeObserver: class {}, RowGlide: class {},
 });
 window.window = window;
 const load = file => vm.runInContext(source(file), window, { filename: file });
 load('i18n.js'); load('backend-protocol.js'); load('backend-client.js'); load('settings.js');
 window.Settings.prototype.pager = () => {};
 window.Settings.prototype.refreshAll = () => {}; // No catalog calls; keep real build/closed handling.
 const list = node(), dialog = node();
 dialog.querySelector = () => list;
 new window.Settings(dialog);
 t.after(() => window.Backend.dispose());
 const deliver = message => ipc.emit('backend:message', { sender: 'must not cross the bridge' }, { jsonrpc: '2.0', ...message });
 const setStatus = value => ipc.emit('backend:status', {}, value);
 const initialize = async () => {
  await tick();
  const init = sent.at(-1);
  assert.equal(init.method, 'initialize');
  deliver({ id: init.id, result: { protocolVersion: '0.1', capabilities: {} } });
  assert.equal(await window.Backend.ready, true);
 };
 return { window, ipc, sent, list, load, deliver, setStatus, initialize };
}

test('preload backend subscriptions strip IPC events and can be removed independently/idempotently', t => {
 const { window, ipc } = page(t, { state: 'none' });
 for (const [method, channel] of [['onMessage', 'backend:message'], ['onStatus', 'backend:status']]) {
  const seen = [], payload = { state: 'none' };
  const off = window.openghost.backend[method]((...args) => seen.push(args));
  const keep = window.openghost.backend[method](() => seen.push('keep'));
  assert.equal(typeof off, 'function');
  ipc.emit(channel, { privileged: true }, payload);
  assert.deepEqual(seen, [[payload], 'keep']);
  off(); off(); seen.length = 0;
  ipc.emit(channel, {}, payload);
  assert.deepEqual(seen, ['keep']);
  keep();
  assert.equal(ipc.listenerCount(channel), 1); // Only BackendClient remains.
 }
});

test('unavailable chat errors and the initial settings page retain configuration/spawn/exit details', async t => {
 for (const [status, diagnostic] of [
  [{ state: 'none' }, /No backend is configured.*OPENGHOST_BACKEND.*backend\.json/],
  [{ state: 'error', error: 'spawn /missing/backend ENOENT' }, /spawn \/missing\/backend ENOENT/],
  [{ state: 'error', error: 'backend.json: invalid command <script>' }, /backend\.json: invalid command <script>/],
  [{ state: 'exited', code: 7 }, /exit code 7/],
  [{ state: 'exited', code: null, signal: 'SIGSEGV' }, /signal SIGSEGV/],
 ]) await t.test(status.error || JSON.stringify(status), async t => {
  const { window, list, sent } = page(t, status);
  const backend = window.Backend;
  assert.equal(await backend.ready, false);
  await assert.rejects(backend.request('models.list'), error => {
   assert.equal(error.code, 'backend_unavailable');
   const message = backend.explain(error).message;
   assert.match(message, diagnostic);
   assert.match(message, /relaunch OpenGhost/i);
   return true;
  });
  assert.match(list.innerHTML, /relaunch OpenGhost/i);
  assert.doesNotMatch(list.innerHTML, /<script>/);
  if (status.error?.includes('<script>')) assert.match(list.innerHTML, /invalid command &lt;script&gt;/);
  assert.equal(sent.length, 0); // No retry/restart requests.
 });
});

test('a failed status lookup surfaces its error instead of claiming no backend is configured', async t => {
 const { window, list } = page(t, Promise.reject(new Error('IPC status lookup failed')));
 assert.equal(await window.Backend.ready, false);
 assert.match(window.Backend.unavailable().message, /IPC status lookup failed/);
 assert.match(list.innerHTML, /IPC status lookup failed.*relaunch OpenGhost/);
});

test('handshake failures remain diagnosable on later requests and in settings', async t => {
 const { window, deliver, sent, list } = page(t);
 await tick();
 deliver({ id: sent.at(-1).id, error: { code: -32000, message: 'Unsupported backend configuration' } });
 assert.equal(await window.Backend.ready, false);
 assert.match(window.Backend.unavailable().message, /initialization failed: Unsupported backend configuration/);
 assert.match(list.innerHTML, /Unsupported backend configuration.*relaunch OpenGhost/);
});

test('a crash keeps its diagnostic in pending calls, active chat turns, later sends and settings', async t => {
 const { window, initialize, setStatus, load, list } = page(t);
 await initialize();
 load('chat.js');
 window.Chat.prototype.newDraft = () => ({});
 window.Chat.prototype.activate = () => {};
 const chat = new window.Chat({ thread: node(), bottom: node() });
 let failure;
 const conv = { reconciled: true, turn: { finish: result => { failure = result.error; } } };
 chat.conversations.set('s', conv);
 const pending = window.Backend.request('models.list');
 setStatus({ state: 'exited', code: 3, signal: 'SIGTERM' });
 await assert.rejects(pending, error => error.code === 'backend_crashed' && /exit code 3, signal SIGTERM/.test(error.message));
 assert.equal(conv.reconciled, false);
 assert.equal(failure.code, 'backend_crashed');
 assert.match(window.Backend.explain(failure).message, /exit code 3, signal SIGTERM.*Relaunch OpenGhost/);
 assert.equal(window.Backend.unavailable().message, failure.message);
 assert.match(list.innerHTML, /exit code 3, signal SIGTERM.*Relaunch OpenGhost/);
 chat.dispose();
});

test('re-evaluating the client releases old subscriptions; synthetic reinitialization does not add more', async t => {
 const { window, ipc, sent, load, initialize, setStatus, deliver } = page(t);
 let oldEvents = 0;
 for (let i = 0; i < 3; i++) {
  await initialize();
  const previous = window.Backend;
  previous.on('models.changed', () => oldEvents++);
  load('backend-client.js');
  assert.equal(previous.disposed, true);
  assert.equal(previous.listeners.size, 0);
  for (const channel of ['backend:message', 'backend:status']) assert.equal(ipc.listenerCount(channel), 1);
 }
 await initialize();
 let events = 0;
 window.Backend.on('models.changed', () => events++);
 setStatus({ state: 'exited', code: 0 });
 // Compatibility only: the production host does not restart the process.
 setStatus({ state: 'running' });
 await initialize();
 deliver({ method: 'models.changed' });
 assert.equal(oldEvents, 0);
 assert.equal(events, 1);
 assert.equal(sent.filter(message => message.method === 'initialize').length, 5);
 assert.equal(sent.some(message => message.method === 'shutdown'), false);
 window.Backend.dispose(); window.Backend.dispose();
 for (const channel of ['backend:message', 'backend:status']) assert.equal(ipc.listenerCount(channel), 0);
});

test('dispose rejects pending calls, aborts reverse work and ignores late replies/status', async t => {
 const { window, sent, initialize, deliver, setStatus } = page(t);
 await initialize();
 const backend = window.Backend;
 let signal, finish;
 backend.handle('host.tool', (_, context) => { signal = context.signal; return new Promise(resolve => { finish = resolve; }); });
 deliver({ id: 'reverse', method: 'host.tool', params: {} });
 const pending = backend.request('models.list');
 backend.dispose();
 await assert.rejects(pending, /disposed/);
 assert.equal(signal.aborted, true);
 assert.equal(backend.pending.size, 0);
 assert.equal(backend.incoming.size, 0);
 assert.equal(backend.handlers.size, 0);
 const count = sent.length;
 finish(null);
 deliver({ method: 'models.changed' }); setStatus({ state: 'running' });
 await tick();
 assert.equal(sent.length, count);
 assert.equal(backend.available, false);
 await assert.rejects(backend.request('models.list'), /disposed/);
});

test('dispose during status lookup, DOM readiness or handshake cannot revive/send from the old client', async t => {
 for (const stage of ['status', 'DOM', 'handshake', 'answered']) await t.test(stage, async t => {
  let loaded;
  const document = stage === 'DOM' ? { readyState: 'loading', addEventListener: (_, fn) => { loaded = fn; } } : undefined;
  const { window, initialize, deliver, sent, ipc } = page(t, undefined, document);
  if (stage !== 'status') await tick();
  if (stage === 'answered') deliver({ id: sent.at(-1).id, result: { protocolVersion: '0.1' } });
  const old = window.Backend;
  old.dispose();
  const count = sent.length;
  loaded?.();
  await tick();
  assert.equal(await old.ready, false);
  assert.equal(old.available, false);
  assert.equal(sent.length, count);
  assert.equal(ipc.listenerCount('backend:message'), 0);
  // A replacement still performs the usual successful handshake on the same transport.
  window.Backend = new window.BackendClient(window.openghost.backend);
  await initialize();
 });
});
