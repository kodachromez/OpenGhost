// Focused F16 regression: real webviews on local data/file pages; no backend/provider/network.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'og-f16-'));
const port = 9400 + Math.floor(Math.random() * 500);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const child = spawn(require('electron'), ['.', '--ozone-platform=headless', '--disable-gpu', `--remote-debugging-port=${port}`], {
 cwd: ROOT, env: { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, '.config'), OPENGHOST_BACKEND: '' }, stdio: ['ignore', 'pipe', 'pipe'],
});
let output = '', ws, next = 0, count = 0;
child.stdout.on('data', data => { output += data; });
child.stderr.on('data', data => { output += data; });
const pending = new Map();
async function until(check, ms = 20000) {
 const end = Date.now() + ms;
 while (Date.now() < end) { try { const value = await check(); if (value) return value; } catch {} await sleep(50); }
 throw new Error(`Timed out\n${output}`);
}
function send(method, params) {
 const id = ++next;
 return new Promise((resolve, reject) => {
  const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${method} timed out`)); }, 20000);
  pending.set(id, { resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } });
  ws.send(JSON.stringify({ id, method, params }));
 });
}
async function evaluate(expression) {
 const answer = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
 if (answer.exceptionDetails) throw new Error(answer.exceptionDetails.exception?.description || answer.exceptionDetails.text);
 return answer.result.value;
}
const call = (name, args = {}) => evaluate(`(async () => {
 const args = ${JSON.stringify(args)};
 const answer = await browserPanel.run(${JSON.stringify(name)}, args, { id: crypto.randomUUID(), signal: new AbortController().signal });
 return HostTools.result(args, answer);
})()`);
async function step(name, work) { await work(); console.log(`PASS ${name}`); count++; }
const url = html => `data:text/html;charset=utf-8,${encodeURIComponent(html)}`;
const fixture = url('<!doctype html><title>F16</title><button id="save" onclick="window.clicks=(window.clicks||0)+1">Save</button><input aria-label="Name"><main id="body">' + 'original text '.repeat(5000) + '</main>');
let observed;
try {
 const target = await until(async () => (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(item => item.url.endsWith('index.html')));
 ws = new WebSocket(target.webSocketDebuggerUrl);
 await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
 ws.onmessage = ({ data }) => {
  const answer = JSON.parse(data), waiter = pending.get(answer.id);
  if (!waiter) return;
  pending.delete(answer.id);
  if (answer.error) waiter.reject(new Error(answer.error.message)); else waiter.resolve(answer.result);
 };
 await until(() => evaluate('document.readyState === "complete" && !!window.browserPanel'));
 await evaluate('browserPanel.setOpen(true)');
 await step('real navigation and snapshot expose stable tab/page identities', async () => {
  observed = await call('browser_navigate', { url: fixture });
  assert.ok(!observed.isError, JSON.stringify(observed));
  assert.equal(typeof observed.data.tabId, 'string');
  assert.equal(typeof observed.data.pageId, 'string');
  assert.equal(observed.data.coverage, 'viewport-dom-heuristic');
 });
 await step('ref click still works on the observed page', async () => {
  const ref = Number(Object.keys(observed.data.refs).find(key => observed.data.refs[key].includes('Save')));
  const clicked = await call('browser_click', { ref, tabId: observed.data.tabId, pageId: observed.data.pageId });
  assert.ok(!clicked.isError, JSON.stringify(clicked));
  assert.equal(await evaluate('browserPanel.active.view.executeJavaScript("window.clicks")'), 1);
 });
 await step('read slices are frozen and report UTF-16 pagination', async () => {
  const first = await call('browser_read', { tabId: observed.data.tabId });
  assert.equal(first.data.hasMore, true);
  assert.equal(first.data.offsetUnit, 'utf16');
  assert.equal(first.data.end, 40000);
  await evaluate(`browserPanel.active.view.executeJavaScript(${JSON.stringify('document.getElementById("body").textContent = "changed text"')})`);
  const next = await call('browser_read', { tabId: first.data.tabId, readId: first.data.readId, start: first.data.end });
  assert.ok(!next.isError, JSON.stringify(next));
  assert.match(next.content[0].text, /original text/);
  assert.equal(next.data.hasMore, false);
  assert.equal(next.data.total, first.data.total);
  const fresh = await call('browser_read', { tabId: first.data.tabId });
  assert.match(fresh.content[0].text, /changed text/);
  const stale = await call('browser_read', { tabId: first.data.tabId, readId: first.data.readId, start: first.data.end });
  assert.equal(stale.data.code, 'stale_read');
 });
 await step('navigation invalidates refs even when the new document reuses ref 1', async () => {
  const fresh = await call('browser_navigate', { tabId: observed.data.tabId, url: url('<button onclick="window.wrong=true">Different</button>') });
  assert.notEqual(fresh.data.pageId, observed.data.pageId);
  const stale = await call('browser_click', { tabId: observed.data.tabId, pageId: observed.data.pageId, ref: 1 });
  assert.equal(stale.isError, true);
  assert.equal(stale.data.code, 'stale_page');
  assert.equal(await evaluate('browserPanel.active.view.executeJavaScript("!!window.wrong")'), false);
 });
 await step('covered refs fail without clicking the covering element', async () => {
  const fresh = await call('browser_navigate', { url: url('<button style="width:200px;height:80px">Covered</button><div onclick="window.wrong=true" style="position:fixed;inset:0;background:white">overlay</div>') });
  const ref = Number(Object.keys(fresh.data.refs).find(key => fresh.data.refs[key].includes('Covered')));
  const result = await call('browser_click', { tabId: fresh.data.tabId, pageId: fresh.data.pageId, ref });
  assert.equal(result.isError, true);
  assert.equal(result.data.code, 'element_covered');
  assert.equal(await evaluate('browserPanel.active.view.executeJavaScript("!!window.wrong")'), false);
 });
 await step('a ref click that navigates still returns the destination snapshot', async () => {
  const fresh = await call('browser_navigate', { url: url('<a href="about:blank">Next</a>') });
  const ref = Number(Object.keys(fresh.data.refs).find(key => fresh.data.refs[key].includes('Next')));
  const result = await call('browser_click', { tabId: fresh.data.tabId, pageId: fresh.data.pageId, ref });
  assert.ok(!result.isError, JSON.stringify(result));
  assert.match(result.content[0].text, /URL: about:blank/);
 });
 await step('screenshot dimensions and scale survive the host result adapter', async () => {
  const shot = await call('browser_screenshot');
  assert.ok(!shot.isError, JSON.stringify(shot));
  assert.ok(shot.data.width > 0 && shot.data.height > 0 && shot.data.scale > 0);
  assert.equal(typeof shot.data.truncated, 'boolean');
  assert.equal(shot.content[1].type, 'image');
 });
 await step('switching tabs during the visible pointer delay prevents the pending input', async () => {
  const fresh = await call('browser_navigate', { url: url('<button onclick="window.wrong=true">Target</button>') });
  await call('browser_tabs', { action: 'new' });
  await call('browser_tabs', { action: 'switch', tabId: fresh.data.tabId });
  await evaluate(`window.savedPoint = browserPanel.point; browserPanel.point = function(x, y) {
   savedPoint.call(this, x, y); this.select(this.tabs.find(tab => tab !== this.active));
  };`);
  const result = await call('browser_click', { tabId: fresh.data.tabId, pageId: fresh.data.pageId, ref: 1 });
  await evaluate('browserPanel.point = savedPoint');
  assert.equal(result.data.code, 'stale_tab');
  assert.equal(await evaluate('browserPanel.tabs[0].view.executeJavaScript("!!window.wrong")'), false);
 });
 await step('wait expiry and navigation failure are explicit tool errors', async () => {
  const waiting = await call('browser_wait', { text: 'absent', seconds: 0.5 });
  assert.equal(waiting.isError, true);
  assert.equal(waiting.data.code, 'wait_timeout');
  const failed = await call('browser_navigate', { url: 'file:///nonexistent-openghost-f16-test.html' });
  assert.equal(failed.isError, true);
  assert.equal(failed.data.code, 'navigation_failed');
 });
 console.log(`${count}/${count} F16 Electron checks passed`);
} finally {
 ws?.close();
 child.kill('SIGTERM');
 await Promise.race([new Promise(resolve => child.once('exit', resolve)), sleep(2000)]);
 if (child.exitCode === null) child.kill('SIGKILL');
 fs.rmSync(home, { recursive: true, force: true });
}
