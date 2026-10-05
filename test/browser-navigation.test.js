'use strict';

// Issue #21: navigation completion, not readable/partial DOM, determines success.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');
const ROOT = path.join(__dirname, '..');
const tick = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => {
 let resolve, reject;
 const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
 return { promise, resolve, reject };
};
const LOAD = 30000, STOP = 15000;
const targets = ['https://requested.invalid/', 'back', 'forward', 'reload'];

function fixture() {
 // Drive hard deadlines explicitly, separately from the 75s operation deadline.
 // Only the short settling delay uses a real (zero-delay) timer.
 const timers = new Map();
 const schedule = (fn, ms) => {
  const token = {};
  timers.set(token, { fn, ms });
  if (ms < 1000) token.native = setTimeout(() => { timers.delete(token); fn(); }, 0);
  return token;
 };
 const clear = token => { clearTimeout(token?.native); timers.delete(token); };
 const module = { exports: {} };
 vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'desktop/browser.js'), 'utf8'), {
  module, require: name => name === 'electron' ? {} : require(name), __dirname: path.join(ROOT, 'desktop'),
  process, Buffer, AbortController, setTimeout: schedule, clearTimeout: clear,
 });
 const Browser = module.exports;
 const guest = new EventEmitter(), host = { isDestroyed: () => false, send() {} };
 const started = deferred(), load = deferred(), loadingChecked = deferred();
 const stats = { snapshots: 0, quiet: 0, stops: 0, dispatches: [] };
 let loading = false;
 const start = target => {
  stats.dispatches.push(target);
  loading = true;
  guest.emit('did-start-navigation', {}, target, false, true);
  started.resolve();
 };
 Object.assign(guest, {
  id: 1, isDestroyed: () => false, setWindowOpenHandler() {}, isLoading: () => { loadingChecked.resolve(); return loading; },
  getURL: () => 'https://requested.invalid/', getTitle: () => 'Loaded page',
  loadURL: target => { start(target); return load.promise; },
  navigationHistory: { canGoBack: () => true, canGoForward: () => true, goBack: () => start('back'), goForward: () => start('forward') },
  reload: () => start('reload'),
  stop: () => {
   stats.stops++; loading = false;
   guest.emit('did-fail-provisional-load', {}, -3, 'ERR_ABORTED', guest.getURL(), true);
   load.reject(Object.assign(new Error('ERR_ABORTED'), { code: 'ERR_ABORTED', errno: -3 }));
  },
  debugger: { isAttached: () => true, attach() {}, sendCommand: async () => ({}) },
  executeJavaScriptInIsolatedWorld: async (world, [{ code }]) => {
   if (code.includes('await (__og.snapshot(')) {
    stats.snapshots++;
    return { ok: { lines: ['Readable partial content'], refs: { 1: 'button' }, skipped: 0, truncated: false,
     scroll: { height: 600, vh: 600, vw: 800, top: 0, below: 0 } } };
   }
   stats.quiet++;
   return { ok: true };
  },
 });
 // stop() may reject this unused promise when exercising history/reload.
 load.promise.catch(() => {});
 Browser.adopt(host, guest);
 const baseline = new Map(guest.eventNames().map(name => [name, guest.listenerCount(name)]));
 const run = (url, signal = new AbortController().signal) => Browser.run('browser_navigate', { url, tab: 1 }, host, signal);
 const complete = ({ inPage = false } = {}) => {
  loading = false;
  guest.emit(inPage ? 'did-navigate-in-page' : 'did-navigate', {}, guest.getURL(), true);
  if (!inPage) guest.emit('did-finish-load');
  guest.emit('did-stop-loading');
  load.resolve();
 };
 const fail = (description = 'ERR_NAME_NOT_RESOLVED', { code = -105, provisional = false, mainFrame = true } = {}) => {
  guest.emit(provisional ? 'did-fail-provisional-load' : 'did-fail-load', {}, code, description, guest.getURL(), mainFrame);
 };
 const expire = ms => {
  const match = [...timers].find(([, timer]) => timer.ms === ms);
  assert.ok(match, `expected an active ${ms}ms deadline`);
  timers.delete(match[0]);
  match[1].fn();
 };
 const cleaned = () => {
  assert.equal(timers.size, 0, 'all operation timers are removed');
  for (const name of new Set([...baseline.keys(), ...guest.eventNames()])) {
   assert.equal(guest.listenerCount(name), baseline.get(name) || 0, `no leaked ${name} listeners`);
  }
 };
 return { guest, run, started: started.promise, loadingChecked: loadingChecked.promise, load, stats, complete, fail, expire, cleaned, setLoading: value => { loading = value; } };
}

for (const target of targets) {
 test(`${target}: successful navigation keeps the normal page snapshot`, async () => {
  const f = fixture(), pending = f.run(target);
  await f.started;
  // An iframe failure must not fail the main document.
  f.fail('ERR_CERT_AUTHORITY_INVALID', { mainFrame: false });
  f.complete();
  const answer = await pending;
  assert.match(answer.text, /Page: Loaded page\nURL: https:\/\/requested.invalid\//);
  assert.match(answer.text, /Readable partial content/);
  assert.equal(answer.refs[1], 'button');
  assert.equal(answer.error, undefined);
  assert.equal(f.stats.snapshots, 1);
  assert.equal(f.stats.stops, 0);
  f.cleaned();
 });

 test(`${target}: hard load timeout is an error even with readable partial DOM; late completion cannot return a snapshot`, async () => {
  const f = fixture(), pending = f.run(target);
  const rejected = assert.rejects(pending, { code: 'timeout', message: 'The page did not load in time' });
  await f.started;
  // Neither committing partial content nor stopping loading establishes success.
  f.guest.emit('did-navigate');
  f.setLoading(false);
  f.guest.emit('did-stop-loading');
  await tick();
  assert.equal(f.stats.snapshots, 0);
  f.expire(LOAD);
  await rejected;
  f.complete();
  await tick();
  assert.equal(f.stats.snapshots, 0);
  assert.equal(f.stats.quiet, 0);
  assert.equal(f.stats.stops, 1);
  f.cleaned();
 });

 test(`${target}: explicit cancellation wins over the ERR_ABORTED caused by stopping the load`, async () => {
  const f = fixture(), controller = new AbortController();
  const pending = f.run(target, controller.signal);
  const rejected = assert.rejects(pending, { code: 'cancelled', message: 'Stopped by the user' });
  await f.started;
  controller.abort();
  await rejected;
  f.complete();
  await tick();
  assert.equal(f.stats.snapshots, 0);
  assert.equal(f.stats.stops, 1);
  f.cleaned();
 });
}

for (const [description, code] of [['ERR_NAME_NOT_RESOLVED', -105], ['ERR_CONNECTION_REFUSED', -102], ['ERR_CERT_AUTHORITY_INVALID', -202], ['ERR_FAILED', -2]]) {
 test(`loadURL ${description} is never normal page state`, async () => {
  const f = fixture(), pending = f.run(targets[0]);
  const rejected = assert.rejects(pending, error => error.code === 'navigation_failed' && error.message.includes(description));
  await f.started;
  f.load.reject(Object.assign(new Error(description), { code: description, errno: code }));
  await rejected;
  assert.equal(f.stats.snapshots, 0);
  assert.equal(f.stats.stops, 1);
  f.cleaned();
 });
}

test('loadURL ERR_ABORTED explicitly fails, rather than assuming a redirect/download completed', async () => {
 for (const error of [Object.assign(new Error('cancelled'), { errno: -3 }), Object.assign(new Error('cancelled'), { code: 'ERR_ABORTED' }), new Error('ERR_ABORTED (-3) loading requested URL')]) {
  const f = fixture(), pending = f.run(targets[0]);
  const rejected = assert.rejects(pending, error => error.code === 'navigation_aborted' && /did not finish loading/.test(error.message));
  await f.started;
  f.load.reject(error);
  await rejected;
  assert.equal(f.stats.snapshots, 0);
  f.cleaned();
 }
});

test('loadURL uses its own completion, not an abort event for the previous superseded load', async () => {
 const f = fixture(), pending = f.run(targets[0]);
 await f.started;
 f.guest.emit('did-fail-provisional-load', {}, -3, 'ERR_ABORTED', 'https://previous.invalid/', true);
 f.guest.emit('did-redirect-navigation', {}, 'https://requested.invalid/redirect', false, true);
 f.complete();
 assert.match((await pending).text, /Readable partial content/);
 assert.equal(f.stats.stops, 0);
 f.cleaned();
});

for (const target of ['back', 'forward', 'reload']) {
 for (const provisional of [false, true]) {
  test(`${target}: ${provisional ? 'provisional' : 'load'} errors and aborts survive later commit/completion events`, async () => {
   for (const [description, code] of [['ERR_CONNECTION_REFUSED', -102], ['ERR_ABORTED', -3]]) {
    const f = fixture(), pending = f.run(target);
    const rejected = assert.rejects(pending, { code: code === -3 ? 'navigation_aborted' : 'navigation_failed' });
    await f.started;
    f.fail(description, { code, provisional });
    f.complete(); // An error page or replacement page cannot erase the failure.
    await rejected;
    assert.equal(f.stats.snapshots, 0);
    assert.equal(f.stats.stops, 1);
    f.cleaned();
   }
  });
 }
}

for (const target of ['back', 'forward']) {
 test(`${target}: same-document history succeeds without did-finish-load`, async () => {
  const f = fixture(), pending = f.run(target);
  await f.started;
  f.complete({ inPage: true });
  assert.match((await pending).text, /Page: Loaded page/);
  assert.equal(f.stats.stops, 0);
  f.cleaned();
 });
 test(`${target}: unavailable history fails without dispatching navigation`, async () => {
  const f = fixture();
  f.guest.navigationHistory.canGoBack = f.guest.navigationHistory.canGoForward = () => false;
  await assert.rejects(f.run(target), /There is no page to go/);
  assert.deepEqual(f.stats.dispatches, []);
  assert.equal(f.stats.snapshots, 0);
  f.cleaned();
 });
}

test('a prevented reload fails rather than returning the old page', async () => {
 const f = fixture(), pending = f.run('reload');
 const rejected = assert.rejects(pending, { code: 'navigation_aborted' });
 await f.started;
 f.guest.emit('will-prevent-unload', {});
 await rejected;
 assert.equal(f.stats.snapshots, 0);
 f.cleaned();
});

test('partial content after load completion cannot turn a settle timeout into success', async () => {
 const f = fixture(), pending = f.run(targets[0]);
 const rejected = assert.rejects(pending, { code: 'timeout', message: 'The page did not finish loading' });
 await f.started;
 f.complete();
 f.setLoading(true); // A subsequent load stalls during settling.
 await f.loadingChecked;
 f.expire(STOP);
 await rejected;
 assert.equal(f.stats.snapshots, 0);
 assert.equal(f.stats.stops, 1);
 f.cleaned();
});

test('main-frame failure during snapshot cannot escape as success or poison the next operation', async () => {
 const f = fixture(), execute = f.guest.executeJavaScriptInIsolatedWorld;
 f.guest.executeJavaScriptInIsolatedWorld = async (world, scripts) => {
  if (scripts[0].code.includes('await (__og.snapshot(')) f.fail('ERR_CONNECTION_RESET', { code: -101 });
  return execute(world, scripts);
 };
 const pending = f.run(targets[0]);
 const rejected = assert.rejects(pending, { code: 'navigation_failed' });
 await f.started;
 f.complete();
 await rejected;
 f.cleaned();
 f.guest.executeJavaScriptInIsolatedWorld = execute;
 f.guest.loadURL = async () => {};
 assert.match((await f.run(targets[0])).text, /Readable partial content/);
 f.cleaned();
});

test('stop cleanup errors do not mask the original navigation error', async () => {
 const f = fixture(), pending = f.run(targets[0]);
 const rejected = assert.rejects(pending, error => error.code === 'navigation_failed' && /ERR_FAILED/.test(error.message));
 await f.started;
 f.guest.stop = () => { throw new Error('guest is gone'); };
 f.load.reject(new Error('ERR_FAILED'));
 await rejected;
 f.cleaned();
});
