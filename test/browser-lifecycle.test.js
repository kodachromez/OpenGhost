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

test('destroyed/replaced readiness rejects and old view events cannot mutate its replacement', async () => {
 for (const reason of ['destroyed', 'replaced', 'closed']) {
  const { panel, add, views, emit } = panelPage();
  const tab = add(), ready = panel.ensure(tab, signal());
  const rejected = assert.rejects(ready, error => error.code === 'tab_gone');
  if (reason === 'destroyed') emit(views[0], 'destroyed');
  if (reason === 'replaced') panel.createView(tab, 'about:blank');
  if (reason === 'closed') panel.close(tab);
  await rejected;
  if (reason === 'closed') {
   assert.equal(tab.state, 'gone');
   emit(views[0], 'dom-ready');
   assert.equal(tab.id, 0);
   continue;
  }
  const next = panel.ensure(tab, signal()), replacement = tab.view;
  for (const event of ['dom-ready', 'render-process-gone', 'destroyed', 'page-title-updated']) emit(views[0], event, { title: 'stale' });
  assert.equal(tab.view, replacement);
  assert.equal(tab.state, 'loading');
  assert.equal(tab.title, '');
  emit(replacement, 'dom-ready');
  await next;
  assert.equal(tab.id, 8);
 }
});

test('readiness timeout is terminal for that view, and a fresh attempt has its own readiness', async () => {
 const { panel, add, views, emit } = panelPage({ cap: 25 });
 const tab = add();
 await assert.rejects(panel.ensure(tab, signal()), error => error.code === 'timeout');
 emit(views[0], 'dom-ready');
 assert.equal(tab.state, 'failed');
 assert.equal(tab.id, 0);
 const next = panel.ensure(tab, signal());
 emit(views[0], 'destroyed');
 emit(views[1], 'dom-ready');
 await next;
 assert.equal(tab.state, 'ready');
 assert.equal(tab.id, 8);
});

test('Stop at dom-ready, before desktop registration, never dispatches the old job', async () => {
 for (const readyFirst of [false, true]) for (const name of ['browser_type', 'browser_tabs']) {
  const runs = [], cancels = [];
  const { panel, add, views, emit } = panelPage({ bridge: {
   shown() {}, cancel: id => cancels.push(id), run: async id => { runs.push(id); return { text: 'fresh' }; },
  } });
  const tab = add(), controller = new AbortController();
  const args = name === 'browser_tabs' ? { action: 'new', url: 'about:blank' } : { text: 'old' };
  const pending = panel.run(name, args, { id: 'old', signal: controller.signal });
  await tick();
  if (readyFirst) emit(views[0], 'dom-ready');
  controller.abort();
  if (!readyFirst) emit(views[0], 'dom-ready');
  assert.equal((await pending).code, 'cancelled');
  await tick();
  assert.deepEqual(runs, []);
  assert.deepEqual(cancels, ['old']);
  await panel.run('browser_snapshot', {}, { id: 'fresh', signal: signal() });
  assert.deepEqual(runs, ['fresh']);
  panel.close(tab);
 }
});

test('closing or replacing a tab cancels active and queued jobs bound to it', async () => {
 for (const replace of [false, true]) {
  const runs = [], cancels = [], gate = deferred();
  const { panel, add, emit } = panelPage({ bridge: {
   shown() {}, cancel: id => cancels.push(id), run: id => { runs.push(id); return gate.promise; },
  } });
  const tab = add(), ready = panel.ensure(tab, signal());
  emit(tab.view, 'dom-ready'); await ready;
  const first = panel.run('browser_wait', {}, { id: 'active', signal: signal() });
  const queued = panel.run('browser_type', { text: 'never' }, { id: 'queued', signal: signal() });
  await tick();
  if (replace) panel.createView(tab, 'about:blank'); else panel.close(tab);
  assert.equal((await first).code, 'tab_gone');
  assert.equal((await queued).code, 'tab_gone');
  gate.resolve({ text: 'late' });
  await tick();
  assert.deepEqual(runs, ['active']);
  assert.deepEqual(cancels.sort(), ['active', 'queued']);
  if (replace) panel.close(tab);
 }
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

function mainBrowser({ cap, onTimer } = {}) {
 const session = new EventEmitter();
 session.setUserAgent = session.setPermissionRequestHandler = session.setPermissionCheckHandler = () => {};
 const image = { getSize: () => ({ width: 800, height: 600 }), toJPEG: () => Buffer.from('image') };
 const electron = { app: { getPath: () => '/tmp' }, session: { fromPartition: () => session }, nativeImage: { createFromBuffer: () => image } };
 const module = { exports: {} };
 vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'desktop/browser.js'), 'utf8'), {
  module, require: name => name === 'electron' ? electron : require(name), __dirname: path.join(ROOT, 'desktop'),
  process, Buffer, AbortController, setTimeout: (fn, ms) => { onTimer?.(ms); return timers(cap)(fn, ms); }, clearTimeout, console,
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
   if (code.includes('await (__og.read(')) return { ok: { html: '<html>original</html>', sourceTruncated: false } };
   return { ok: true };
  },
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

test('uncovered snapshot refs dispatch normal single and double clicks at the resolved point', async () => {
 for (const double of [false, true]) {
  const { Browser } = mainBrowser();
  const { run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  const answer = await run('browser_click', { ref: 1, pageId, double });
  const expected = [{ method: 'Input.dispatchMouseEvent', params: { x: 10, y: 20, type: 'mouseMoved' } }];
  for (let clickCount = 1; clickCount <= (double ? 2 : 1); clickCount++) {
   expected.push(
    { method: 'Input.dispatchMouseEvent', params: { x: 10, y: 20, type: 'mousePressed', button: 'left', buttons: 1, clickCount } },
    { method: 'Input.dispatchMouseEvent', params: { x: 10, y: 20, type: 'mouseReleased', button: 'left', buttons: 0, clickCount } },
   );
  }
  assert.deepEqual(plain(sent.filter(item => item.method.startsWith('Input.'))), expected);
  assert.notEqual(answer.pageId, pageId);
 }
});

test('covered snapshot refs identify the obstruction and recovery options without any mouse input', async () => {
 for (const coverAt of [1, 2]) for (const double of [false, true]) {
  const { Browser } = mainBrowser();
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  let points = 0;
  guest.executeJavaScriptInIsolatedWorld = async (world, [{ code }]) => {
   assert.ok(code.includes('await (__og.point(1))'));
   return { ok: { x: 10, y: 20, covered: ++points >= coverAt ? 'button "Overlay"' : '' } };
  };
  // Explicit coordinates must not become a fallback when a ref is covered.
  await assert.rejects(run('browser_click', { ref: 1, pageId, double, x: 30, y: 40 }), error => {
   assert.equal(error.code, 'element_covered');
   assert.match(error.message, /Element \[1\] is covered by button "Overlay"/);
   assert.match(error.message, /No click was sent/);
   assert.match(error.message, /Take a fresh snapshot or explicitly target the covering element/);
   return true;
  });
  assert.equal(points, coverAt);
  assert.deepEqual(sent.filter(item => item.method.startsWith('Input.')), []);
 }
});

test('coordinate clicks without a ref dispatch at the supplied point without resolving an element', async () => {
 const { Browser } = mainBrowser();
 const { guest, run, sent } = guestPair(Browser);
 const { pageId } = await run('browser_snapshot');
 const execute = guest.executeJavaScriptInIsolatedWorld;
 let points = 0;
 guest.executeJavaScriptInIsolatedWorld = async (world, scripts) => {
  if (scripts[0].code.includes('await (__og.point(')) {
   points++;
   return { ok: { x: 10, y: 20, covered: 'overlay' } };
  }
  return execute(world, scripts);
 };
 const answer = await run('browser_click', { x: 30, y: 40, pageId });
 assert.equal(points, 0);
 assert.deepEqual(plain(sent.filter(item => item.method.startsWith('Input.'))), [
  { method: 'Input.dispatchMouseEvent', params: { x: 30, y: 40, type: 'mouseMoved' } },
  { method: 'Input.dispatchMouseEvent', params: { x: 30, y: 40, type: 'mousePressed', button: 'left', buttons: 1, clickCount: 1 } },
  { method: 'Input.dispatchMouseEvent', params: { x: 30, y: 40, type: 'mouseReleased', button: 'left', buttons: 0, clickCount: 1 } },
 ]);
 assert.notEqual(answer.pageId, pageId);
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
  guest.executeJavaScriptInIsolatedWorld = () => new Promise(() => {});
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

test('Stop interrupts each click/type command boundary and ignores late acknowledgements', { timeout: 5000 }, async () => {
 const cases = [
  ['browser_click', { x: 10, y: 20, double: true }, 5],
  ['browser_type', { ref: 1, text: 'secret', submit: true }, 8],
  ['browser_type', { text: '', submit: true }, 6],
 ];
 for (const [name, args, stages] of cases) for (let stage = 1; stage <= stages; stage++) {
  const { Browser } = mainBrowser();
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  const gate = deferred(), reached = deferred(), controller = new AbortController();
  let inputs = 0;
  guest.debugger.sendCommand = (method, params) => {
   sent.push({ method, params });
   if (method.startsWith('Input.') && ++inputs === stage) { reached.resolve(); return gate.promise; }
   return Promise.resolve({});
  };
  const pending = run(name, { ...args, pageId }, controller.signal);
  const rejected = assert.rejects(pending, error => error.code === 'cancelled');
  await reached.promise;
  controller.abort();
  await rejected;
  assert.equal(inputs, stage, `${name} stopped at stage ${stage}`);
  gate.resolve({});
  await tick();
  assert.equal(inputs, stage, 'late CDP completion must not continue the operation');
 }
});

test('plain wait, pointer, post-click and pre-submit delays are cancellation-aware', { timeout: 2000 }, async () => {
 for (const [name, args, delay] of [
  ['browser_wait', { seconds: 60 }, 250],
  ['browser_click', { ref: 1 }, 420],
  ['browser_type', { ref: 1, text: 'secret', submit: true }, 80],
  ['browser_type', { text: 'secret', submit: true }, 60],
 ]) {
  const reached = deferred(), controller = new AbortController();
  let armed = false;
  const { Browser } = mainBrowser({ onTimer: ms => { if (armed && ms === delay) reached.resolve(); } });
  const { run, sent } = guestPair(Browser);
  Browser.setShown({ open: delay === 420, id: 1 });
  const { pageId } = await run('browser_snapshot');
  armed = true;
  const pending = run(name, { ...args, pageId }, controller.signal);
  const rejected = assert.rejects(pending, error => error.code === 'cancelled');
  await reached.promise;
  const before = sent.length;
  controller.abort();
  await rejected;
  await tick();
  assert.equal(sent.length, before);
  assert.equal(sent.some(item => item.params.key === 'Enter'), false);
 }
});

test('Take Control during type retires clear/insert/submit stages before granting UI ownership', { timeout: 2000 }, async () => {
 for (const boundary of ['mouseReleased', 'keyUp', 'insert']) {
  const { Browser } = mainBrowser();
  const { guest, host, run, sent } = guestPair(Browser, 7);
  const { pageId } = await run('browser_snapshot');
  const gate = deferred(), reached = deferred(), acknowledgement = deferred();
  const controller = new AbortController();
  let desktop, settled = false, focused = false;
  const { panel, add } = panelPage({ bridge: {
   shown() {},
   run: (id, name, args) => desktop = Browser.run(name, args, host, controller.signal).catch(error => ({ error: error.message, code: error.code })).finally(() => { settled = true; }),
   cancel: async () => { controller.abort(); await desktop; await acknowledgement.promise; },
  } });
  const tab = add();
  tab.id = 7; tab.state = 'ready'; tab.view = { focus() { assert.ok(settled); focused = true; }, blur() {} }; tab.ready = Promise.resolve(tab);
  guest.debugger.sendCommand = (method, params) => {
   sent.push({ method, params });
   if (params.type === boundary || (boundary === 'insert' && method === 'Input.insertText')) { reached.resolve(); return gate.promise; }
   return Promise.resolve({});
  };
  const active = panel.run('browser_type', { ref: 1, pageId, text: 'secret', submit: true }, { id: 'type', signal: signal() });
  const queued = panel.run('browser_type', { pageId, text: 'never' }, { id: 'queued', signal: signal() });
  await reached.promise;
  const before = sent.length, taking = panel.take();
  assert.equal(panel.control, 'taking');
  assert.equal(focused, false);
  panel.handBack();
  assert.equal(panel.control, 'taking', 'Hand Back cannot skip the cancellation acknowledgement');
  assert.equal((await active).taken, true);
  assert.equal((await queued).taken, true);
  acknowledgement.resolve();
  await taking;
  assert.equal(panel.control, 'user');
  assert.ok(focused);
  panel.handBack();
  gate.resolve({});
  await tick();
  assert.equal(sent.length, before, 'Hand Back and late CDP completion cannot revive the old type');
  assert.equal(sent.some(item => item.params.key === 'Enter'), false);
 }
});

test('Stop interrupts navigation, read, snapshot, scroll, select and screenshot stages', { timeout: 2000 }, async () => {
 for (const name of ['browser_navigate', 'browser_read', 'browser_snapshot', 'browser_scroll', 'browser_select', 'browser_screenshot']) {
  const { Browser } = mainBrowser();
  const { guest, run, sent } = guestPair(Browser);
  const { pageId } = await run('browser_snapshot');
  const gate = deferred(), reached = deferred(), controller = new AbortController();
  let stopped = 0, dispatched = 0;
  const stall = () => { dispatched++; reached.resolve(); return gate.promise; };
  guest.stop = () => stopped++;
  guest.loadURL = guest.executeJavaScript = guest.executeJavaScriptInIsolatedWorld = stall;
  guest.debugger.sendCommand = stall;
  const pending = run(name, { pageId, ref: 1, option: 'x', url: 'about:blank' }, controller.signal);
  const rejected = assert.rejects(pending, error => error.code === 'cancelled');
  await reached.promise;
  controller.abort(); await rejected;
  const before = sent.length;
  gate.resolve({}); await tick();
  assert.equal(dispatched, 1);
  assert.equal(sent.length, before);
  assert.equal(stopped, name === 'browser_navigate' ? 1 : 0);
 }
});

test('cancelling a queued main job never releases or bypasses its predecessor', async () => {
 const { Browser } = mainBrowser();
 const { guest, run } = guestPair(Browser);
 const { pageId } = await run('browser_snapshot');
 const gate = deferred(), second = new AbortController();
 let reads = 0;
 guest.executeJavaScriptInIsolatedWorld = () => { reads++; return reads === 1 ? gate.promise : Promise.resolve({ ok: { html: 'fresh', sourceTruncated: false } }); };
 const first = run('browser_read');
 const cancelled = run('browser_type', { pageId, text: 'never' }, second.signal);
 const rejected = assert.rejects(cancelled, error => error.code === 'cancelled');
 const third = run('browser_read');
 second.abort(); await rejected; await tick();
 assert.equal(reads, 1);
 gate.resolve({ ok: { html: 'first', sourceTruncated: false } }); await first; await third;
 assert.equal(reads, 2);
});

test('read continuation uses frozen HTML, rejects missing/expired identity and exposes source truncation', async () => {
 const { Browser } = mainBrowser();
 const { guest, run } = guestPair(Browser);
 const first = await run('browser_read');
 guest.executeJavaScriptInIsolatedWorld = async () => ({ ok: { html: '<html>changed</html>', sourceTruncated: false } });
 const next = await run('browser_read', { start: 2, readId: first.readId });
 assert.equal(next.html, first.html);
 await assert.rejects(run('browser_read', { start: 2 }), error => error.code === 'stale_read');
 guest.emit('did-start-navigation', {}, 'data:text/html,new', false, true);
 await assert.rejects(run('browser_read', { start: 2, readId: first.readId }), error => error.code === 'stale_read');
 guest.executeJavaScriptInIsolatedWorld = async () => ({ ok: { html: 'x'.repeat(4 * 1024 * 1024), sourceTruncated: true } });
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
