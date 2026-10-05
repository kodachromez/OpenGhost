// Issue #21: navigation outcomes on local fixtures, through generic tool IPC.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { createServer } from 'node:http';
const require = createRequire(import.meta.url);
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'og-f16-'));
const port = 9400 + Math.floor(Math.random() * 500);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const requests = new Set();
const server = createServer((req, res) => {
 requests.add(req.url);
 if (req.url.startsWith('/pending') || req.url.startsWith('/stalled')) return;
 if (req.url === '/redirect') { res.writeHead(302, { location: '/ok?redirected' }); res.end(); return; }
 if (req.url === '/reset') { req.socket.destroy(); return; }
 res.setHeader('Content-Type', 'text/html');
 res.setHeader('Cache-Control', 'no-store');
 if (req.url === '/partial') res.end('<title>Partial</title><main>Readable but unfinished navigation</main><img src="/stalled">');
 else res.end(`<title>Navigation ${req.url}</title><main>Navigation ${req.url}</main>`);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const local = `http://127.0.0.1:${server.address().port}`;
const child = spawn(require('electron'), ['.', '--ozone-platform=headless', '--disable-gpu', `--remote-debugging-port=${port}`], {
 cwd: ROOT, env: { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, '.config') }, stdio: ['ignore', 'pipe', 'pipe'],
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
  const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${method} timed out`)); }, 45000);
  pending.set(id, { resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } });
  ws.send(JSON.stringify({ id, method, params }));
 });
}
async function evaluate(expression) {
 const answer = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
 if (answer.exceptionDetails) throw new Error(answer.exceptionDetails.exception?.description || answer.exceptionDetails.text);
 return answer.result.value;
}
const call = (name, args = {}) => evaluate(`browserPanel.run(${JSON.stringify(name)}, ${JSON.stringify(args)}, { id: crypto.randomUUID(), signal: new AbortController().signal })`);
async function step(name, work) { await work(); console.log(`PASS ${name}`); count++; }
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
 await step('missing-file failure stays an error through AgentTools', async () => {
  const result = await evaluate(`AgentTools.run('browser_navigate', {url:'file:///nonexistent-openghost-issue21.html'}, {id:'missing-file'})`);
  assert.match(result, /^Error: .*ERR_/);
 });
 await step('navigation redirect, back, forward and reload retain successful snapshots', async () => {
  const first = await call('browser_navigate', { url: `${local}/ok?first` });
  assert.ok(!first.error, JSON.stringify(first));
  const second = await call('browser_navigate', { url: `${local}/ok?second` });
  assert.ok(!second.error, JSON.stringify(second));
  for (const [url, expected] of [['back', 'first'], ['forward', 'second'], ['reload', 'second']]) {
   const result = await call('browser_navigate', { url });
   assert.ok(!result.error, JSON.stringify(result));
   assert.ok((result.error || result.text).includes(`${local}/ok?${expected}`));
  }
  const redirected = await call('browser_navigate', { url: `${local}/redirect` });
  assert.ok(!redirected.error, JSON.stringify(redirected));
  assert.ok(redirected.text.includes(`${local}/ok?redirected`));
  await call('browser_navigate', { url: `${local}/ok?redirected#anchor` });
  for (const url of ['back', 'forward']) {
   const result = await call('browser_navigate', { url });
   assert.ok(!result.error, JSON.stringify(result));
   assert.equal((result.error || result.text).includes('#anchor'), url === 'forward');
  }
 });
 await step('reload of the committed page can supersede a pending load without misattributing its abort', async () => {
  await evaluate(`window.previousLoad = browserPanel.active.view.loadURL(${JSON.stringify(`${local}/pending-reload`)}).catch(() => {}); true`);
  await until(() => requests.has('/pending-reload'));
  const result = await call('browser_navigate', { url: 'reload' });
  assert.ok(!result.error, JSON.stringify(result));
  assert.ok((result.error || result.text).includes(`${local}/ok?redirected#anchor`), JSON.stringify(result));
 });
 await step('network load failure is an error through generic IPC', async () => {
  const result = await call('browser_navigate', { url: `${local}/reset` });
  assert.equal(typeof result.error, 'string');
  assert.equal(result.code, 'navigation_failed');
  assert.match((result.error || result.text), /ERR_/);
 });
 await step('stopping a pending load returns failure, not the previous page snapshot', async () => {
  await evaluate(`window.navigationProbe = browserPanel.run('browser_navigate', { url: ${JSON.stringify(`${local}/pending`)} },
   { id: 'issue21-abort', signal: new AbortController().signal }); true`);
  await until(() => requests.has('/pending'));
  await evaluate('browserPanel.active.view.stop(); true');
  const result = await evaluate('window.navigationProbe');
  assert.equal(typeof result.error, 'string');
  // Electron may reject loadURL with ERR_FAILED rather than ERR_ABORTED on stop.
  assert.ok(['navigation_aborted', 'navigation_failed'].includes(result.code), JSON.stringify(result));
  assert.match((result.error || result.text), /ERR_(ABORTED|FAILED)/);
 });
 await step('readable partial page still fails at the real hard load deadline', async () => {
  await evaluate(`window.navigationProbe = browserPanel.run('browser_navigate', { url: ${JSON.stringify(`${local}/partial`)} },
   { id: 'issue21-partial', signal: new AbortController().signal }); true`);
  await until(() => requests.has('/stalled'));
  await until(() => evaluate('browserPanel.active.view.executeJavaScript("document.body.textContent.includes(\\"Readable but unfinished navigation\\")")'));
  const result = await evaluate('window.navigationProbe');
  assert.equal(typeof result.error, 'string');
  assert.equal(result.code, 'timeout');
  assert.match((result.error || result.text), /^The page did not load in time/);
  assert.equal(result.refs, undefined, 'partial content was not returned as a successful navigation');
  const next = await call('browser_navigate', { url: `${local}/ok?after-timeout` });
  assert.ok(!next.error, JSON.stringify(next));
 });
 console.log(`${count}/${count} browser Electron checks passed`);
} finally {
 ws?.close();
 server.closeAllConnections();
 server.close();
 child.kill('SIGTERM');
 await Promise.race([new Promise(resolve => child.once('exit', resolve)), sleep(2000)]);
 if (child.exitCode === null) child.kill('SIGKILL');
 fs.rmSync(home, { recursive: true, force: true });
}
