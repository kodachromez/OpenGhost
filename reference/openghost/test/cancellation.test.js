'use strict';

// Stop and Take Control against the browser host tool (audit F02): the chat's host.tool handler and the browser panel run
// together on a fake bridge, and the main process's browser steps on a fake guest. Nothing here opens a window.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const Module = require('node:module');
const { EventEmitter } = require('node:events');
const { ROOT, tick } = require('./helpers');

const deferred = () => {
 let resolve;
 const promise = new Promise(done => { resolve = done; });
 return { promise, resolve };
};

// The bridge to the main process: each run waits until the test answers it; a cancel answers it as the main process does
// for a step it stopped before its next action.
function fakeBridge() {
 const bridge = { runs: [], cancels: [] };
 bridge.run = (id, name, args) => {
  const job = { id, name, args, ...deferred() };
  bridge.runs.push(job);
  return job.promise;
 };
 bridge.cancel = id => {
  bridge.cancels.push(id);
  bridge.runs.find(job => job.id === id)?.resolve({ error: 'Stopped by the user', stopped: true });
 };
 return bridge;
}

// chat.js, browser-panel.js and host-tools.js in one page-like context, with a chat and a panel built without the DOM.
function page() {
 const bridge = fakeBridge(), requests = [];
 const window = {
  console, setTimeout, clearTimeout, setImmediate, queueMicrotask, AbortController, DOMException,
  document: { activeElement: null },
  openghost: { browser: bridge },
  Backend: { request: (method, params) => { requests.push({ method, params }); return Promise.resolve({}); }, on() {}, handle() {} },
 };
 window.window = window;
 const context = vm.createContext(window);
 for (const file of ['host-tools.js', 'browser-panel.js', 'chat.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), context, { filename: file });
 }
 const panel = Object.create(window.BrowserPanel.prototype);
 const tab = { id: 7, view: { focus() {}, blur() {} } };
 Object.assign(panel, { drivers: new Set(), control: 'agent', waiters: [], jobs: new Map(), tabs: [tab], active: tab, lent: null, gate: null });
 panel.sync = () => {};
 panel.giveBack = () => {};
 panel.ensure = async () => { await panel.gate; return tab; };
 window.browserPanel = panel;
 const chat = () => Object.assign(Object.create(window.Chat.prototype), { showGhost() {}, dismissGhost() {} });
 const conversation = (owner, id = 'c1') => {
  const conv = { id };
  const turn = owner.begin(conv, {});
  turn.remote = 't1';
  turn.part = { view: {} };
  return conv;
 };
 return { window, bridge, panel, chat, conversation, requests };
}

const plain = value => JSON.parse(JSON.stringify(value));
const CANCELLED = { status: 'cancelled', content: [] };
const request = (name, args = {}) => ({ sessionId: 's1', turnId: 't1', toolCallId: `call-${Math.random()}`, name, args });

test('Stop answers every browser step of the turn at once, cancels each, and drops what they still bring back', async () => {
 const { bridge, panel, chat, conversation, requests } = page();
 const owner = chat(), conv = conversation(owner);
 const first = owner.onHostTool(conv, request('browser_wait', { seconds: 2 }), new AbortController().signal);
 const second = owner.onHostTool(conv, request('browser_type', { ref: 1, text: 'hi' }), new AbortController().signal);
 await tick();
 assert.deepEqual(bridge.runs.map(job => job.name), ['browser_wait']); // F16 serializes queued input across chats.
 const ids = [...panel.jobs.keys()];
 const late = bridge.runs.map(job => job.resolve);
 bridge.cancel = id => bridge.cancels.push(id); // The main process is slow to answer: the chat must not wait for it.
 owner.abort(conv);
 assert.deepEqual(plain(await first), CANCELLED);
 assert.deepEqual(plain(await second), CANCELLED);
 assert.deepEqual(bridge.cancels.sort(), ids.sort());
 assert.deepEqual(requests.map(item => item.method), ['turn.cancel']);
 for (const resolve of late) resolve({ text: 'Page: late' });
 await tick();
 assert.equal(bridge.runs.length, 1);
});

test('a step stopped before it reaches the browser never runs there', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner), gate = deferred();
 panel.gate = gate.promise;
 const answer = owner.onHostTool(conv, request('browser_click', { ref: 1 }), new AbortController().signal);
 await tick();
 owner.abort(conv);
 gate.resolve();
 assert.deepEqual(plain(await answer), CANCELLED);
 await tick();
 assert.equal(bridge.runs.length, 0);
});

test('a tabs step stopped while its new tab loads does not navigate it', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner), gate = deferred();
 panel.newTab = () => { const tab = { id: 8 }; panel.tabs.push(tab); return tab; };
 panel.gate = gate.promise;
 const answer = owner.onHostTool(conv, request('browser_tabs', { action: 'new', url: 'example.com' }), new AbortController().signal);
 await tick();
 owner.abort(conv);
 gate.resolve();
 assert.deepEqual(plain(await answer), CANCELLED);
 await tick();
 assert.equal(bridge.runs.length, 0);
});

test('the backend cancelling one request stops that step only', async () => {
 const { bridge, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner), cancel = new AbortController();
 const first = owner.onHostTool(conv, request('browser_wait'), cancel.signal);
 const second = owner.onHostTool(conv, request('browser_snapshot'), new AbortController().signal);
 await tick();
 cancel.abort();
 assert.deepEqual(plain(await first), CANCELLED);
 assert.deepEqual(bridge.cancels, [bridge.runs[0].id]);
 await tick();
 bridge.runs[1].resolve({ text: 'Page: still here' });
 const done = await second;
 assert.equal(done.status, 'ok');
 assert.match(done.content[0].text, /still here/);
});

test('browser steps of a main chat and its side chat never share an id', async () => {
 const { bridge, panel, chat, conversation } = page();
 const main = chat(), side = chat();
 const first = new AbortController(), second = new AbortController();
 const one = main.onHostTool(conversation(main, 'c1'), request('browser_snapshot'), first.signal);
 const two = side.onHostTool(conversation(side, 'c1'), request('browser_snapshot'), second.signal);
 await tick();
 assert.equal(bridge.runs.length, 1);
 assert.equal(panel.jobs.size, 2, 'both the running and queued call have unique ids');
 first.abort(); second.abort();
 await Promise.all([one, two]);
});

test('Take Control stops the step under way; it waits for the hand-back and reports the page instead', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner);
 const answer = owner.onHostTool(conv, request('browser_click', { ref: 3 }), new AbortController().signal);
 await tick();
 const [click] = bridge.runs;
 panel.take();
 assert.deepEqual(bridge.cancels, [click.id]);
 await tick();
 assert.equal(bridge.runs.length, 1, 'nothing more runs while the user has the browser');
 panel.handBack();
 await tick();
 assert.deepEqual(bridge.runs.map(job => job.name), ['browser_click', 'browser_snapshot']);
 bridge.runs[1].resolve({ text: 'Page: as the user left it' });
 const done = await answer;
 assert.equal(done.status, 'handed-back');
 assert.match(done.content[0].text, /as the user left it/);
});

test('Take Control before a step reaches the browser keeps it from running', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner), gate = deferred();
 panel.gate = gate.promise;
 const answer = owner.onHostTool(conv, request('browser_type', { ref: 2, text: 'secret', submit: true }), new AbortController().signal);
 await tick();
 panel.take();
 gate.resolve();
 await tick();
 assert.equal(bridge.runs.length, 0);
 panel.gate = null;
 panel.handBack();
 await tick();
 assert.deepEqual(bridge.runs.map(job => job.name), ['browser_snapshot']);
 bridge.runs[0].resolve({ text: 'Page: x' });
 assert.equal((await answer).status, 'handed-back');
});

test('a step the main process finished before the take keeps its real result', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner);
 const answer = owner.onHostTool(conv, request('browser_click', { ref: 3 }), new AbortController().signal);
 await tick();
 bridge.runs[0].resolve({ text: 'Page: clicked' });
 bridge.cancel = id => bridge.cancels.push(id);
 panel.take();
 const done = await answer;
 assert.equal(done.status, 'ok');
 assert.match(done.content[0].text, /clicked/);
});

test('Stop answers every step waiting for the user to hand the browser back', async () => {
 const { bridge, panel, chat, conversation } = page();
 const owner = chat(), conv = conversation(owner);
 panel.drive(conv, true);
 panel.take();
 const waiting = [
  owner.onHostTool(conv, request('browser_click', { ref: 1 }), new AbortController().signal),
  owner.onHostTool(conv, request('browser_type', { ref: 2, text: 'x' }), new AbortController().signal),
 ];
 await tick();
 owner.abort(conv);
 for (const answer of waiting) assert.deepEqual(plain(await answer), CANCELLED);
 panel.handBack();
 await tick();
 assert.equal(bridge.runs.length, 0);
});

// The main process's browser steps, on a fake guest: electron is stubbed while desktop/browser.js loads.
function mainBrowser() {
 const load = Module._load;
 Module._load = function (request, ...rest) {
  if (request === 'electron') return { app: {}, clipboard: {}, Menu: {}, nativeImage: {}, session: {}, webContents: {} };
  return load.call(this, request, ...rest);
 };
 try {
  const file = path.join(ROOT, 'desktop', 'browser.js');
  delete require.cache[file];
  return require(file);
 } finally {
  Module._load = load;
 }
}

function fakeGuest(id, onPointer) {
 const sent = [], guest = new EventEmitter();
 Object.assign(guest, {
  id,
  isDestroyed: () => false,
  setWindowOpenHandler() {},
  isLoading: () => false,
  getTitle: () => 'Test',
  getURL: () => 'data:text/html,test',
  executeJavaScriptInIsolatedWorld: async (world, [{ code }]) => ({ ok: code.includes('await (__og.snapshot(')
   ? { lines: [], refs: {}, scroll: { height: 600, vh: 600, vw: 800, top: 0, below: 0 } } : { x: 10, y: 20 } }),
  debugger: { isAttached: () => true, attach() {}, sendCommand: async (method, params) => { sent.push({ method, params }); return {}; } },
 });
 const host = { isDestroyed: () => false, send: (channel, data) => { if (data?.type === 'pointer') onPointer(); } };
 return { guest, host, sent };
}

test('a click or typing stopped while the cursor moves to the page presses nothing', async () => {
 const Browser = mainBrowser();
 for (const [name, args] of [['browser_click', { ref: 1 }], ['browser_type', { ref: 1, text: 'secret', submit: true }]]) {
  const controller = new AbortController();
  const { guest, host, sent } = fakeGuest(name === 'browser_click' ? 501 : 502, () => controller.abort());
  Browser.adopt(host, guest);
  const { pageId } = await Browser.run('browser_snapshot', { tab: guest.id }, host, controller.signal);
  await assert.rejects(Browser.run(name, { ...args, pageId, tab: guest.id }, host, controller.signal), /Stopped by the user/);
  const input = sent.filter(item => item.method.startsWith('Input.'));
  assert.deepEqual(input, [], `${name} sent no input after the stop`);
 }
});

test('a stopped wait ends at once instead of holding the tab', async () => {
 const Browser = mainBrowser();
 const { guest, host } = fakeGuest(503, () => {});
 Browser.adopt(host, guest);
 const controller = new AbortController(), started = Date.now();
 const waiting = Browser.run('browser_wait', { seconds: 60, tab: guest.id }, host, controller.signal);
 setTimeout(() => controller.abort(), 50);
 await assert.rejects(waiting, /Stopped by the user/);
 assert.ok(Date.now() - started < 2000);
});
