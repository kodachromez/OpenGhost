// Browser regressions: issue #21 and F16 real webview lifecycle; no backend/provider/external network.
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
 await step('issue #21: encoded URL markup stays literal in the real address-bar DOM', async () => {
  assert.equal(await evaluate('browserPanel.open'), false);
  const address = 'https://example.invalid/%3Cb%20id=%22injected%22%3Ehello%3C/b%3E';
  const result = await evaluate(`(() => {
   // A closed panel keeps this tab lazy: exercise the actual UI without navigating to the URL.
   const tab = browserPanel.newTab(${JSON.stringify(address)}, { focus: false });
   try {
    const view = browserPanel.urlView;
    return {
     input: browserPanel.url.value, text: view.textContent,
     pieces: [...view.children].map(el => [el.tagName, el.className, el.textContent]),
     textOnly: [...view.children].every(el => el.childNodes.length === 1 && el.firstChild.nodeType === Node.TEXT_NODE),
     injected: !!document.getElementById('injected'),
     guestCreated: !!tab.view,
     staticIcons: ['.browser-new', '.browser-back', '.browser-forward', '.browser-reload', '.browser-external', '.browser-tab-close']
      .every(selector => !!browserPanel.root.querySelector(selector + ' svg path')),
    };
   } finally { browserPanel.close(tab); }
  })()`);
  assert.equal(result.input, address);
  assert.equal(result.text, 'example.invalid/<b id="injected">hello</b>');
  assert.deepEqual(result.pieces, [
   ['SPAN', 'browser-url-host', 'example.invalid'],
   ['SPAN', 'browser-url-dim', '/<b id="injected">hello</b>'],
  ]);
  assert.equal(result.textOnly, true);
  assert.equal(result.injected, false);
  assert.equal(result.guestCreated, false);
  assert.equal(result.staticIcons, true, 'trusted static SVG/UI markup still renders');
  assert.equal(await evaluate('browserPanel.urlView.childNodes.length'), 0, 'closing the tab clears the address');
 });
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
  const fresh = await call('browser_navigate', { url: url('<script>window.mouseEvents=[]; for (const type of ["mousedown","mouseup","click"]) document.addEventListener(type, () => window.mouseEvents.push(type));</script><button style="width:200px;height:80px">Covered</button><div onclick="window.wrong=true" style="position:fixed;inset:0;background:white">overlay</div>') });
  const ref = Number(Object.keys(fresh.data.refs).find(key => fresh.data.refs[key].includes('Covered')));
  const result = await call('browser_click', { tabId: fresh.data.tabId, pageId: fresh.data.pageId, ref });
  assert.equal(result.isError, true);
  assert.equal(result.data.code, 'element_covered');
  assert.ok(result.content[0].text.includes(`Element [${ref}] is covered by`));
  assert.match(result.content[0].text, /overlay/);
  assert.match(result.content[0].text, /Take a fresh snapshot or explicitly target the covering element/);
  assert.deepEqual(await evaluate('browserPanel.active.view.executeJavaScript("window.mouseEvents")'), []);
  assert.equal(await evaluate('browserPanel.active.view.executeJavaScript("!!window.wrong")'), false);
 });
 await step('coordinate clicks still reach the covering element when no ref is supplied', async () => {
  const fresh = await call('browser_snapshot');
  const result = await call('browser_click', { tabId: fresh.data.tabId, pageId: fresh.data.pageId, x: 20, y: 20 });
  assert.ok(!result.isError, JSON.stringify(result));
  assert.deepEqual(await evaluate('browserPanel.active.view.executeJavaScript("window.mouseEvents")'), ['mousedown', 'mouseup', 'click']);
  assert.equal(await evaluate('browserPanel.active.view.executeJavaScript("!!window.wrong")'), true);
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
 await step('Take Control cancels real typing before clear/insert/submit and waits for desktop acknowledgement', async () => {
  const fresh = await call('browser_navigate', { url: url('<form onsubmit="window.submits=(window.submits||0)+1;event.preventDefault()"><input id="field" aria-label="Name" value="user value"><button>Send</button></form>') });
  const ref = Number(Object.keys(fresh.data.refs).find(key => fresh.data.refs[key].includes('Name')));
  await evaluate(`browserPanel.drive('lifecycle-test', true);
   window.savedPoint = browserPanel.point;
   browserPanel.point = function(x, y) {
    savedPoint.call(this, x, y);
    window.takeAck = this.take();
    window.controlBeforeAck = this.control;
    window.userBeforeAck = this.root.classList.contains('is-user');
   };`);
  const result = await evaluate(`browserPanel.run('browser_type', ${JSON.stringify({ ref, pageId: fresh.data.pageId, text: 'old agent input', submit: true })}, { id: 'real-taken-type', signal: new AbortController().signal })`);
  assert.equal(result.taken, true);
  await evaluate('takeAck');
  assert.deepEqual(await evaluate('[controlBeforeAck, userBeforeAck, browserPanel.control, browserPanel.root.classList.contains("is-user")]'), ['taking', false, 'user', true]);
  await evaluate('browserPanel.point = savedPoint; browserPanel.handBack()');
  await sleep(600); // Past pointer and type delays: the retired request must stay retired.
  assert.deepEqual(await evaluate('browserPanel.active.view.executeJavaScript("[document.getElementById(\\"field\\").value, window.submits || 0]")'), ['user value', 0]);
  const snapshot = await call('browser_snapshot');
  const typed = await call('browser_type', { ref: Number(Object.keys(snapshot.data.refs).find(key => snapshot.data.refs[key].includes('Name'))), pageId: snapshot.data.pageId, text: 'fresh agent input', submit: true });
  assert.ok(!typed.isError, JSON.stringify(typed));
  assert.deepEqual(await evaluate('browserPanel.active.view.executeJavaScript("[document.getElementById(\\"field\\").value, window.submits || 0]")'), ['fresh agent input', 1]);
  await evaluate('browserPanel.drive("lifecycle-test", false)');
 });
 await step('Stop interrupts a real plain wait and a cancelled queued type never starts', async () => {
  const result = await evaluate(`(async () => {
   const first = new AbortController(), second = new AbortController();
   const active = browserPanel.run('browser_wait', { seconds: 60 }, { id: 'real-wait', signal: first.signal });
   const queued = browserPanel.run('browser_type', { text: 'must not type', submit: true }, { id: 'real-queued', signal: second.signal });
   await new Promise(resolve => setTimeout(resolve, 100));
   const at = performance.now();
   second.abort(); first.abort();
   const answers = await Promise.all([active, queued]);
   await Promise.all(browserPanel.cancelling.values());
   return { codes: answers.map(answer => answer.code), elapsed: performance.now() - at };
  })()`);
  assert.deepEqual(result.codes, ['cancelled', 'cancelled']);
  assert.ok(result.elapsed < 5000, JSON.stringify(result));
  assert.deepEqual(await evaluate('browserPanel.active.view.executeJavaScript("[document.getElementById(\\"field\\").value, window.submits || 0]")'), ['fresh agent input', 1]);
 });
 await step('Stop before real dom-ready registration cannot revive the job later', async () => {
  const result = await evaluate(`(async () => {
   const tab = browserPanel.newTab('', { focus: false });
   const controller = new AbortController();
   const original = browserPanel.createView;
   // Stop synchronously after the real webview is inserted, before its first dom-ready.
   browserPanel.createView = function(...args) { const view = original.apply(this, args); controller.abort(); return view; };
   try {
    const answer = await browserPanel.run('browser_navigate', { url: 'data:text/html,SHOULD-NOT-LOAD' }, { id: 'real-before-ready', signal: controller.signal });
    await tab.ready;
    await new Promise(resolve => setTimeout(resolve, 100));
    return { code: answer.code, url: tab.view.getURL() };
   } finally { browserPanel.createView = original; browserPanel.close(tab); }
  })()`);
  assert.equal(result.code, 'cancelled');
  assert.equal(result.url, 'about:blank');
 });
 await step('replacing a real pending webview rejects the old readiness and readies only the replacement', async () => {
  const result = await evaluate(`(async () => {
   const tab = browserPanel.newTab('', { focus: false });
   try {
    const old = browserPanel.ensure(tab).then(() => 'unexpected success', error => error.code);
    browserPanel.createView(tab, 'about:blank');
    const replacement = tab.view;
    await browserPanel.ensure(tab);
    return { old: await old, ready: tab.state, same: tab.view === replacement };
   } finally { browserPanel.close(tab); }
  })()`);
  assert.deepEqual(result, { old: 'tab_gone', ready: 'ready', same: true });
 });
 await step('wait expiry and navigation failure are explicit tool errors', async () => {
  const waiting = await call('browser_wait', { text: 'absent', seconds: 0.5 });
  assert.equal(waiting.isError, true);
  assert.equal(waiting.data.code, 'wait_timeout');
  const failed = await call('browser_navigate', { url: 'file:///nonexistent-openghost-f16-test.html' });
  assert.equal(failed.isError, true);
  assert.equal(failed.data.code, 'navigation_failed');
 });
 await step('navigation redirect, back, forward and reload retain successful snapshots', async () => {
  const first = await call('browser_navigate', { url: `${local}/ok?first` });
  assert.ok(!first.isError, JSON.stringify(first));
  const second = await call('browser_navigate', { url: `${local}/ok?second` });
  assert.ok(!second.isError, JSON.stringify(second));
  for (const [url, expected] of [['back', 'first'], ['forward', 'second'], ['reload', 'second']]) {
   const result = await call('browser_navigate', { url });
   assert.ok(!result.isError, JSON.stringify(result));
   assert.ok(result.content[0].text.includes(`${local}/ok?${expected}`));
  }
  const redirected = await call('browser_navigate', { url: `${local}/redirect` });
  assert.ok(!redirected.isError, JSON.stringify(redirected));
  assert.ok(redirected.content[0].text.includes(`${local}/ok?redirected`));
  await call('browser_navigate', { url: `${local}/ok?redirected#anchor` });
  for (const url of ['back', 'forward']) {
   const result = await call('browser_navigate', { url });
   assert.ok(!result.isError, JSON.stringify(result));
   assert.equal(result.content[0].text.includes('#anchor'), url === 'forward');
  }
 });
 await step('reload of the committed page can supersede a pending load without misattributing its abort', async () => {
  await evaluate(`window.previousLoad = browserPanel.active.view.loadURL(${JSON.stringify(`${local}/pending-reload`)}).catch(() => {}); true`);
  await until(() => requests.has('/pending-reload'));
  const result = await call('browser_navigate', { url: 'reload' });
  assert.ok(!result.isError, JSON.stringify(result));
  assert.ok(result.content[0].text.includes(`${local}/ok?redirected#anchor`), JSON.stringify(result));
 });
 await step('network load failure is an error through IPC and HostTools.result', async () => {
  const result = await call('browser_navigate', { url: `${local}/reset` });
  assert.equal(result.isError, true);
  assert.equal(result.data.code, 'navigation_failed');
  assert.match(result.content[0].text, /^Error: .*ERR_/);
 });
 await step('stopping a pending load returns failure, not the previous page snapshot', async () => {
  await evaluate(`window.navigationProbe = browserPanel.run('browser_navigate', { url: ${JSON.stringify(`${local}/pending`)} },
   { id: 'issue21-abort', signal: new AbortController().signal }).then(answer => HostTools.result({}, answer)); true`);
  await until(() => requests.has('/pending'));
  await evaluate('browserPanel.active.view.stop(); true');
  const result = await evaluate('window.navigationProbe');
  assert.equal(result.isError, true);
  // Electron may reject loadURL with ERR_FAILED rather than ERR_ABORTED on stop.
  assert.ok(['navigation_aborted', 'navigation_failed'].includes(result.data.code), JSON.stringify(result));
  assert.match(result.content[0].text, /ERR_(ABORTED|FAILED)/);
 });
 await step('readable partial page still fails at the real hard load deadline', async () => {
  await evaluate(`window.navigationProbe = browserPanel.run('browser_navigate', { url: ${JSON.stringify(`${local}/partial`)} },
   { id: 'issue21-partial', signal: new AbortController().signal }).then(answer => HostTools.result({}, answer)); true`);
  await until(() => requests.has('/stalled'));
  await until(() => evaluate('browserPanel.active.view.executeJavaScript("document.body.textContent.includes(\\"Readable but unfinished navigation\\")")'));
  const result = await evaluate('window.navigationProbe');
  assert.equal(result.isError, true);
  assert.equal(result.data.code, 'timeout');
  assert.match(result.content[0].text, /^Error: The page did not load in time/);
  assert.equal(result.data.refs, undefined, 'partial content was not returned as a successful navigation');
  const next = await call('browser_navigate', { url: `${local}/ok?after-timeout` });
  assert.ok(!next.isError, JSON.stringify(next));
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
