'use strict';

// F16: controlled renderer lifecycle and main-process guests, with no backend or network.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const { randomUUID } = require('node:crypto');
const { ROOT, tick } = require('./helpers');
const plain = value => JSON.parse(JSON.stringify(value));
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
const signal = () => new AbortController().signal;
const timers = cap => (fn, ms) => setTimeout(fn, cap && ms >= 1000 ? cap : ms);

function panelPage({ cap, bridge = { run: async () => ({ text: 'ok' }), shown() {}, cancel() {} } } = {}) {
 const views = [], notices = [];
 const document = { activeElement: null, createElement() {
  const view = new EventTarget();
  Object.assign(view, { classList: { toggle() {} }, setAttribute() {}, getWebContentsId: () => 7 + views.indexOf(view),
   remove() {}, getURL: () => 'about:blank', getTitle: () => '', blur() {}, focus() {} });
  views.push(view);
  return view;
 } };
 const window = { openghost: { browser: bridge }, Backend: { available: true, notify: (name, params) => notices.push({ name, params }) } };
 const context = vm.createContext({ window, document, AbortController, crypto: { randomUUID }, setTimeout: timers(cap), clearTimeout, queueMicrotask, console });
 for (const file of ['host-tools.js', 'browser-panel.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), context);
 const panel = Object.assign(Object.create(window.BrowserPanel.prototype), {
  tabs: [], active: null, open: false, control: 'agent', drivers: new Set(), jobs: new Map(), queue: Promise.resolve(), accounts: [], waiters: [],
  stage: { insertBefore() {} }, error: {}, render() {}, syncBar() {}, save() {}, giveBack() {}, sync() {},
 });
 const add = () => { const tab = panel.addTab(''); panel.active = tab; return tab; };
 const emit = (view, name, props = {}) => view.dispatchEvent(Object.assign(new Event(name), props));
 return { panel, add, views, emit, bridge, notices, window };
}

test('discovery and browser context distinguish unavailable from available but empty', () => {
 const absent = panelPage({ bridge: null });
 assert.equal(absent.window.HostTools.schemas.length, 0);
 assert.equal(absent.panel.snapshot().status, 'unavailable');
 const { panel, window, notices } = panelPage();
 assert.equal(window.HostTools.schemas.length, 11);
 assert.equal(panel.snapshot().status, 'empty');
 assert.equal(panel.snapshot().available, true);
 panel.report();
 panel.report();
 assert.equal(notices.length, 1);
 assert.equal(notices[0].name, 'host.browser.changed');
 assert.equal(notices[0].params.browser.signedInVerified, false);
});

test('first readiness rejects on close, failed load, crash, timeout and cancellation', async () => {
 for (const reason of ['close', 'failed', 'crash', 'timeout', 'cancel']) {
  const { panel, add, views, emit } = panelPage({ cap: 25 });
  const tab = add(), controller = new AbortController();
  const ready = panel.ensure(tab, controller.signal);
  const expected = { close: 'tab_gone', failed: 'navigation_failed', crash: 'guest_crashed', timeout: 'timeout', cancel: 'cancelled' }[reason];
  const rejected = assert.rejects(ready, error => error.code === expected);
  if (reason === 'close') panel.close(tab);
  if (reason === 'failed') emit(views[0], 'did-fail-load', { isMainFrame: true, errorCode: -105, errorDescription: 'failed' });
  if (reason === 'crash') emit(views[0], 'render-process-gone');
  if (reason === 'cancel') controller.abort(Object.assign(new Error('cancelled'), { code: 'cancelled' }));
  await rejected;
  tab.failReady?.(new Error('test cleanup'));
 }
});

test('a crashed guest has explicit gone state and is recreated on the next readiness attempt', async () => {
 const { panel, add, views, emit } = panelPage();
 const tab = add(), first = panel.ensure(tab, signal());
 emit(views[0], 'dom-ready');
 await first;
 emit(views[0], 'render-process-gone');
 assert.equal(panel.snapshot().status, 'gone');
 const next = panel.ensure(tab, signal());
 assert.equal(views.length, 2);
 emit(views[1], 'dom-ready');
 assert.equal(await next, tab);
 assert.equal(panel.snapshot().status, 'ready');
});

test('serialized calls pin the receipt tab, and queued cancellation never reaches the bridge', async () => {
 const gate = deferred(), runs = [];
 const { panel, add } = panelPage({ bridge: { shown() {}, cancel() {}, run: (id, name, args) => { runs.push(args); return gate.promise; } } });
 const tab = add(); tab.id = 7; tab.view = {}; tab.ready = Promise.resolve(tab); tab.state = 'ready';
 const first = panel.run('browser_snapshot', {}, { id: 'a', signal: signal() });
 const controller = new AbortController();
 const second = panel.run('browser_snapshot', {}, { id: 'b', signal: controller.signal });
 const third = panel.run('browser_snapshot', {}, { id: 'c', signal: signal() });
 await tick();
 assert.equal(runs.length, 1);
 controller.abort();
 assert.equal((await second).code, 'cancelled');
 add();
 gate.resolve({ text: 'first' });
 await first;
 assert.equal((await third).code, 'stale_tab');
 assert.equal(runs.length, 1);
});

test('abort while readying a new tab cannot dispatch later, and the queue is released', async () => {
 const { panel, views, emit } = panelPage();
 const controller = new AbortController();
 const run = panel.run('browser_tabs', { action: 'new', url: 'data:text/html,x' }, { id: 'new', signal: controller.signal });
 await tick();
 controller.abort();
 assert.equal((await run).code, 'cancelled');
 emit(views[0], 'dom-ready');
 const next = await panel.run('browser_tabs', { action: 'list' }, { id: 'list', signal: signal() });
 assert.equal(next.tabs.length, 1);
});

test('tab mutations require stable handles and opening at capacity never evicts', async () => {
 const { panel, add } = panelPage();
 for (let k = 0; k < 12; k++) add();
 const handles = panel.tabs.map(tab => tab.handle);
 const call = args => panel.run('browser_tabs', args, { id: randomUUID(), signal: signal() });
 assert.equal((await call({ action: 'new' })).code, 'tab_limit');
 assert.deepEqual(panel.tabs.map(tab => tab.handle), handles);
 assert.equal((await call({ action: 'close', tab: 1 })).code, 'invalid_request');
 await call({ action: 'close', tabId: handles[0] });
 assert.equal(panel.tabs.length, 11);
 assert.equal((await call({ action: 'close', tabId: handles[0] })).code, 'tab_gone');
 assert.equal((await call({ action: 'typo' })).code, 'invalid_request');
});

function mainBrowser({ cap } = {}) {
 const session = new EventEmitter();
 session.setUserAgent = session.setPermissionRequestHandler = session.setPermissionCheckHandler = () => {};
 const image = { getSize: () => ({ width: 800, height: 600 }), toJPEG: () => Buffer.from('image') };
 const electron = { app: { getPath: () => '/tmp' }, session: { fromPartition: () => session }, nativeImage: { createFromBuffer: () => image } };
 const module = { exports: {} };
 vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'desktop/browser.js'), 'utf8'), {
  module, require: name => name === 'electron' ? electron : require(name), __dirname: path.join(ROOT, 'desktop'),
  process, Buffer, AbortController, setTimeout: timers(cap), clearTimeout, console,
 });
 return { Browser: module.exports, session };
}

function guestPair(Browser, id = 1) {
 const guest = new EventEmitter(), sent = [];
 const host = { isDestroyed: () => false, send() {} };
 Object.assign(guest, { id, hostWebContents: host, isDestroyed: () => false, setWindowOpenHandler() {}, isLoading: () => false,
  getURL: () => 'data:text/html,test', getTitle: () => 'test', stop() {},
  executeJavaScriptInIsolatedWorld: async (world, [{ code }]) => {
   if (code.includes('await (__og.snapshot(')) return { ok: { lines: ['page'], refs: { 1: 'button' }, skipped: 0, truncated: true, scroll: { height: 600, vh: 600, vw: 800, top: 0, below: 0 } } };
   if (code.includes('await (__og.point(')) return { ok: { x: 10, y: 20, covered: '' } };
   if (code.includes('await (__og.has(')) return { ok: false };
   return { ok: true };
  },
  executeJavaScript: async () => '<html>original</html>',
  debugger: { isAttached: () => true, attach() {}, sendCommand: async (method, params) => {
   sent.push({ method, params });
   if (method === 'Page.getLayoutMetrics') return { cssVisualViewport: { clientWidth: 800, clientHeight: 600 }, cssContentSize: { height: 6000 } };
   return { data: '' };
  } },
 });
 Browser.adopt(host, guest);
 const run = (name, args = {}, sig = signal()) => Browser.run(name, { ...args, tab: id }, host, sig);
 return { guest, host, run, sent };
}

test('page identity rejects missing/stale refs and navigation during the cursor delay sends no input', async () => {
 const { Browser } = mainBrowser();
 const { guest, host, run, sent } = guestPair(Browser);
 const observed = await run('browser_snapshot');
 await assert.rejects(run('browser_click', { ref: 1 }), error => error.code === 'stale_page');
 guest.emit('did-start-navigation', {}, 'data:text/html,new', false, true);
 await assert.rejects(run('browser_click', { ref: 1, pageId: observed.pageId }), error => error.code === 'stale_page');
 const current = await run('browser_snapshot');
 host.send = () => guest.emit('did-start-navigation', {}, 'data:text/html,redirect', false, true);
 await assert.rejects(run('browser_click', { ref: 1, pageId: current.pageId }), error => error.code === 'stale_page');
 assert.equal(sent.filter(item => item.method.startsWith('Input.')).length, 0);
});

test('covered and moved targets fail before clicking or typing', async () => {
 for (const name of ['browser_click', 'browser_type']) for (const moved of [false, true]) {
  const { Browser } = mainBrowser();
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  let points = 0;
  guest.executeJavaScriptInIsolatedWorld = async () => ({ ok: { x: moved ? 10 + points++ : 10, y: 20, covered: moved ? '' : 'overlay' } });
  await assert.rejects(run(name, { ref: 1, text: 'secret', pageId }), error => ['element_covered', 'stale_target'].includes(error.code));
  assert.equal(sent.filter(item => item.method.startsWith('Input.')).length, 0);
 }
});

test('navigation before the final input acknowledgement is allowed, but never permits another press', async () => {
 for (const times of [1, 2]) {
  const { Browser } = mainBrowser();
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  guest.debugger.sendCommand = async (method, params) => {
   sent.push({ method, params });
   if (params.type === 'keyDown') {
    guest.emit('did-start-navigation', {}, 'data:text/html,next', false, true);
    guest.emit('did-navigate');
   }
   return {};
  };
  const pending = run('browser_press', { key: 'Enter', pageId, times });
  if (times === 1) assert.notEqual((await pending).pageId, pageId);
  else await assert.rejects(pending, error => error.code === 'stale_page');
  assert.equal(sent.filter(item => item.params?.type === 'keyDown').length, 1);
 }
});

test('overlapping input from one observation cannot act after another call changes the page state', async () => {
 const { Browser } = mainBrowser();
 const { run, sent } = guestPair(Browser);
 const { pageId } = await run('browser_snapshot');
 const first = run('browser_press', { key: 'Enter', pageId });
 const second = run('browser_press', { key: 'Enter', pageId });
 const rejected = assert.rejects(second, error => error.code === 'stale_page');
 const answer = await first;
 await rejected;
 assert.notEqual(answer.pageId, pageId);
 assert.equal(sent.filter(item => item.method === 'Input.dispatchKeyEvent').length, 2);
});

test('guest crash/destruction interrupts a stalled call, and a navigation commit invalidates an in-load observation', async () => {
 for (const event of ['render-process-gone', 'destroyed']) {
  const { Browser } = mainBrowser();
  const { guest, run } = guestPair(Browser);
  guest.executeJavaScript = () => new Promise(() => {});
  const pending = run('browser_read');
  await tick();
  guest.emit(event);
  await assert.rejects(pending, error => error.code === (event === 'destroyed' ? 'tab_gone' : 'guest_crashed'));
 }
 const { Browser } = mainBrowser();
 const { guest, run } = guestPair(Browser);
 guest.emit('did-start-navigation', {}, 'data:text/html,next', false, true);
 const { pageId } = await run('browser_snapshot');
 guest.emit('did-navigate');
 await assert.rejects(run('browser_click', { x: 1, y: 1, pageId }), error => error.code === 'stale_page');
});

test('cancelled queue entries do not dispatch even while an earlier guest command is stalled', async () => {
 const { Browser } = mainBrowser();
 const { guest, run, sent } = guestPair(Browser);
 const gate = deferred();
 guest.debugger.sendCommand = () => gate.promise;
 const first = new AbortController(), second = new AbortController();
 const running = run('browser_snapshot', {}, first.signal);
 const queued = run('browser_press', { key: 'Enter', pageId: 'old' }, second.signal);
 const stopped = assert.rejects(queued, error => error.code === 'cancelled');
 second.abort();
 await stopped;
 first.abort();
 await assert.rejects(running, error => error.code === 'cancelled');
 gate.resolve({});
 await tick();
 assert.deepEqual(sent, []);
});

test('CDP and navigation timeouts cannot resume into input; navigation failures and wait expiry are errors', async () => {
 {
  const { Browser } = mainBrowser({ cap: 25 });
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  const gate = deferred();
  guest.debugger.sendCommand = () => gate.promise;
  await assert.rejects(run('browser_press', { pageId, key: 'Enter' }), error => error.code === 'timeout');
  gate.resolve({});
  await tick();
  assert.equal(sent.filter(item => item.method.startsWith('Input.')).length, 0);
 }
 {
  const { Browser } = mainBrowser({ cap: 25 });
  const { guest, run } = guestPair(Browser);
  let stopped = 0;
  guest.loadURL = () => new Promise(() => {});
  guest.stop = () => stopped++;
  await assert.rejects(run('browser_navigate', { url: 'data:text/html,x' }), error => error.code === 'timeout');
  assert.equal(stopped, 1);
 }
 {
  const { Browser } = mainBrowser();
  const { guest, run } = guestPair(Browser);
  guest.loadURL = async () => { throw new Error('ERR_FAILED'); };
  await assert.rejects(run('browser_navigate', { url: 'data:text/html,x' }), error => error.code === 'navigation_failed');
  await assert.rejects(run('browser_wait', { text: 'absent', seconds: 0.5 }), error => error.code === 'wait_timeout');
 }
});

test('read continuation uses frozen HTML, rejects missing/expired identity and exposes source truncation', async () => {
 const { Browser } = mainBrowser();
 const { guest, run } = guestPair(Browser);
 const first = await run('browser_read');
 guest.executeJavaScript = async () => '<html>changed</html>';
 const next = await run('browser_read', { start: 2, readId: first.readId });
 assert.equal(next.html, first.html);
 await assert.rejects(run('browser_read', { start: 2 }), error => error.code === 'stale_read');
 guest.emit('did-start-navigation', {}, 'data:text/html,new', false, true);
 await assert.rejects(run('browser_read', { start: 2, readId: first.readId }), error => error.code === 'stale_read');
 guest.executeJavaScript = async () => 'x'.repeat(4 * 1024 * 1024 + 1);
 const capped = await run('browser_read');
 assert.equal(capped.sourceTruncated, true);
 assert.equal(capped.html.length, 4 * 1024 * 1024);
});

test('result formatting preserves error, page, snapshot and screenshot metadata', async () => {
 const { Browser } = mainBrowser();
 const { run } = guestPair(Browser);
 const { window } = panelPage();
 const snapshot = window.HostTools.result({}, await run('browser_snapshot'));
 assert.equal(snapshot.data.truncated, true);
 assert.equal(typeof snapshot.data.pageId, 'string');
 const shot = window.HostTools.result({}, await run('browser_screenshot', { full_page: true }));
 assert.equal(shot.data.scale, 1);
 assert.equal(shot.data.width, 800);
 assert.equal(shot.data.pageHeight, 2400);
 assert.equal(shot.data.truncated, true);
 const error = window.HostTools.result({}, { error: 'not found', code: 'wait_timeout' });
 assert.equal(error.isError, true);
 assert.equal(error.data.code, 'wait_timeout');
});

test('downloads from another guest or completed after an operation are not consumed by a later call', async () => {
 const { Browser, session } = mainBrowser();
 Browser.setup();
 const one = guestPair(Browser, 1), two = guestPair(Browser, 2);
 const item = new EventEmitter();
 Object.assign(item, { setSavePath() {}, getFilename: () => 'f16-test-download.txt', getSavePath: () => '/tmp/f16-test-download.txt' });
 session.emit('will-download', {}, item, one.guest);
 item.emit('done', {}, 'completed');
 assert.deepEqual(plain((await two.run('browser_snapshot')).downloads), []);
 assert.deepEqual(plain((await one.run('browser_snapshot')).downloads), []);
 one.guest.loadURL = async () => { session.emit('will-download', {}, item, one.guest); item.emit('done', {}, 'completed'); };
 const completed = await one.run('browser_navigate', { url: 'data:text/html,download', operationId: 'download-one' });
 assert.equal(completed.downloads.length, 1);
 assert.equal(completed.downloads[0].operationId, 'download-one');
 assert.deepEqual(plain((await two.run('browser_snapshot')).downloads), []);
 assert.deepEqual(plain((await one.run('browser_snapshot')).downloads), []);
});
