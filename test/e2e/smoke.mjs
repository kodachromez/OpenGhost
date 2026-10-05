// End-to-end smoke test of the frontend-only app: the real Electron app, headless (no window on screen), driven over the
// Chrome DevTools Protocol, with its own HOME so nothing of the user's is read or written.
//
//   1. No backend: the UI comes up whole, and a message gets "No backend is connected" in the chat, not a reply.
//   2. The scripted test backend (test/fixtures/scripted-backend.js, no AI behind it): providers and models reach the
//      settings and the picker, a reply streams in, an approval card is answered, the browser host tool runs, usage is
//      counted, the backend names the chat, Stop, an auth error, a manual compaction and a backend crash all show as they should.
//
// Run with `npm run test:e2e`. It needs no display: Electron runs with --ozone-platform=headless. To test a packaged
// build instead of the code, set OPENGHOST_E2E_APP to its executable (dist/linux-unpacked/openghost).
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const ELECTRON = path.join(ROOT, 'node_modules', 'electron', 'dist', process.platform === 'win32' ? 'electron.exe' : 'electron');
const FIXTURE = path.join(ROOT, 'test', 'fixtures', 'scripted-backend.js');
const SCRATCH = path.join(os.homedir(), '.cache', 'openghost-e2e');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function waitFor(what, check, timeout = 15000) {
 const end = Date.now() + timeout;
 let last;
 while (Date.now() < end) {
  try {
   last = await check();
   if (last) return last;
  } catch (error) {
   last = error;
  }
  await sleep(100);
 }
 throw new Error(`Timed out waiting for ${what}${last instanceof Error ? `: ${last.message}` : ''}`);
}

class Page {
 constructor(url) {
  this.ws = new WebSocket(url);
  this.next = 1;
  this.waiting = new Map();
  this.errors = [];
  this.ws.onmessage = ({ data }) => {
   const message = JSON.parse(data);
   if (message.id) {
    const waiting = this.waiting.get(message.id);
    this.waiting.delete(message.id);
    if (message.error) waiting?.reject(new Error(message.error.message));
    else waiting?.resolve(message.result);
   } else if (message.method === 'Runtime.exceptionThrown') {
    this.errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
   } else if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error') {
    this.errors.push(message.params.args.map(arg => arg.value ?? arg.description).join(' '));
   }
  };
  this.open = new Promise((resolve, reject) => { this.ws.onopen = resolve; this.ws.onerror = reject; });
 }

 send(method, params = {}) {
  const id = this.next++;
  this.ws.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => this.waiting.set(id, { resolve, reject }));
 }

 // Runs an expression in the page and gives back its value.
 async eval(expression) {
  const { result, exceptionDetails } = await this.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (exceptionDetails) throw new Error(exceptionDetails.exception?.description || exceptionDetails.text);
  return result.value;
 }

 close() {
  this.ws.close();
 }
}

async function launch(name, env = {}) {
 const home = fs.mkdtempSync(path.join(SCRATCH, `${name}-`));
 const port = 9300 + Math.floor(Math.random() * 600);
 const packaged = process.env.OPENGHOST_E2E_APP || '';
 const child = spawn(packaged || ELECTRON, [...(packaged ? [] : ['.']), '--ozone-platform=headless', `--remote-debugging-port=${port}`, '--disable-gpu'], {
  cwd: ROOT,
  env: { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, '.config'), OPENGHOST_BACKEND: '', ...env },
  stdio: ['ignore', 'pipe', 'pipe'],
 });
 let output = '';
 child.stdout.on('data', chunk => { output += chunk; });
 child.stderr.on('data', chunk => { output += chunk; });
 const target = await waitFor('the app window', async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  return list.find(item => item.type === 'page' && item.url.endsWith('index.html'));
 }, 30000).catch(error => { child.kill('SIGKILL'); throw new Error(`${error.message}\n${output}`); });
 const page = new Page(target.webSocketDebuggerUrl);
 await page.open;
 await page.send('Runtime.enable');
 await waitFor('the page to load', () => page.eval("document.readyState === 'complete' && typeof chat === 'object'"));
 const stop = async () => {
  page.close();
  child.kill('SIGTERM');
  await Promise.race([new Promise(resolve => child.once('exit', resolve)), sleep(5000)]);
  if (child.exitCode === null) child.kill('SIGKILL');
  fs.rmSync(home, { recursive: true, force: true });
 };
 return { page, stop, output: () => output };
}

// Types a message into the composer and presses Enter, as the user would.
const send = (page, text) => page.eval(`(() => {
 const input = document.querySelector('.composer-input');
 input.value = ${JSON.stringify(text)};
 input.dispatchEvent(new Event('input', { bubbles: true }));
 input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
 return true;
})()`);
const lastReply = page => page.eval("[...document.querySelectorAll('.thread-list:not(.is-parked) .message.is-assistant')].at(-1)?.textContent || ''");
// A reply has ended once its last part has its toolbar or its note: both come after the text has finished appearing.
const idle = page => waitFor('the reply to end', () => page.eval(`!chat.busy && !document.querySelector('.thread-list:not(.is-parked) .message.is-streaming')`));
const lastOf = (page, selector) => page.eval(`[...document.querySelectorAll('.thread-list:not(.is-parked) ${selector}')].at(-1)?.textContent || ''`);

const results = [];
async function step(name, run) {
 try {
  await run();
  results.push(['PASS', name]);
  console.log(`PASS ${name}`);
 } catch (error) {
  results.push(['FAIL', name]);
  console.log(`FAIL ${name}\n  ${String(error.stack || error).split('\n').slice(0, 4).join('\n  ')}`);
 }
}

fs.mkdirSync(SCRATCH, { recursive: true });

// 1. No backend at all.
{
 const app = await launch('none');
 const { page } = app;
 await step('the UI comes up with no backend', async () => {
  for (const selector of ['.sidebar', '.composer-input', '.composer-model', '.composer-effort', '.composer-add', '.welcome', '.settings', '.browser-toggle'])
   assert.equal(await page.eval(`!!document.querySelector(${JSON.stringify(selector)})`), true, selector);
  assert.equal(await page.eval('Backend.available'), false);
  assert.equal(await page.eval('Backend.state'), 'unavailable');
  assert.equal(await page.eval('typeof AgentTools + typeof Providers + typeof DeepSeek'), 'undefinedundefinedundefined');
 });
 await step('a message says no backend is connected', async () => {
  await send(page, 'Hello there');
  await waitFor('the error', () => page.eval("document.querySelector('.message-error')?.textContent"));
  assert.match(await page.eval("document.querySelector('.message-error').textContent"), /No backend is connected/);
  assert.equal(await page.eval("document.querySelector('.message.is-user .message-bubble')?.textContent"), 'Hello there');
  assert.equal(await page.eval("[...document.querySelectorAll('.message-action')].map(b => b.textContent).join(',')"), 'Retry');
  assert.equal(await page.eval('library.chats.length'), 1);
 });
 await step('the settings say no backend is connected', async () => {
  await page.eval("settings.open('x'); true");
  assert.match(await page.eval("document.querySelector('.settings-providers').textContent"), /No backend is connected/);
  await page.eval("settings.dialog.close(); true");
 });
 await step('no page errors without a backend', async () => assert.deepEqual(page.errors, []));
 await app.stop();
}

// 2. The scripted test backend.
{
 const app = await launch('scripted', { OPENGHOST_BACKEND: JSON.stringify([process.execPath, FIXTURE]) });
 const { page } = app;
 await step('the backend is greeted with the host tools and the render guide', async () => {
  await waitFor('the backend', () => page.eval('Backend.available'));
  const hello = await page.eval("Backend.request('test.hello')");
  assert.equal(hello.protocolVersion, '0.1');
  assert.equal(hello.host.tools.length, 11);
  assert.ok(hello.host.renderGuide.includes('mermaid'));
 });
 await step('providers and models come from the backend', async () => {
  await waitFor('the models', () => page.eval("settings.models.length === 1 && settings.providers.length === 1"));
  assert.equal(await page.eval("settings.models[0].name"), 'Test model');
  assert.deepEqual(await page.eval("settings.efforts"), ['none', 'low', 'high']);
  assert.match(await page.eval("document.querySelector('.settings-providers .provider-name').textContent"), /Test provider/);
  assert.equal(await page.eval("document.querySelector('.settings-key').placeholder"), 'Saved · type to replace');
 });
 await step('missing model metadata stays unknown while explicit capabilities remain visible', async () => {
  const metadata = await page.eval(`(async () => {
   try {
    await Backend.request('test.models', { models: [
     { id: 'm1', provider: 'test', name: 'Unknown', thinkingLevels: [], defaultThinking: 'high' },
     { id: 'text', provider: 'test', name: 'Text', vision: false },
     { id: 'image', provider: 'test', name: 'Image', vision: true, contextWindow: 128000, thinkingLevels: ['medium', 'high'], defaultThinking: 'medium' },
    ] });
    await settings.refresh();
    modelStage.build();
    return {
     labels: modelStage.rows.map(row => row.getAttribute('aria-label')),
     meta: modelStage.rows.map(row => row.querySelector('.model-meta').textContent),
     vision: settings.models.map(model => settings.configFor(model.id).vision),
     efforts: settings.config.efforts, thinkingOmitted: settings.config.effort === undefined,
     effortHidden: document.querySelector('.composer-effort').hidden,
     window: settings.windowOf('test:m1'),
     reported: settings.configFor('test:image').efforts,
    };
   } finally {
    await Backend.request('test.models');
    await settings.refresh();
    modelStage.build();
   }
  })()`);
  assert.deepEqual(metadata, {
   labels: ['Unknown', 'Text, No photos', 'Image, 128K context · Sees photos'],
   meta: ['', 'No photos', '128K context · Sees photos'],
   vision: [null, false, true], efforts: [], thinkingOmitted: true, effortHidden: true, window: 0,
   reported: ['medium', 'high'],
  });
 });
 await step('a reply streams in, an approval is answered and the browser host tool runs', async () => {
  await send(page, 'hello approve browser');
  await waitFor('the approval card', () => page.eval("!!document.querySelector('.approval .approval-button.is-allow')"));
  assert.equal(await page.eval("document.querySelector('.approval-title').textContent"), 'List the files here');
  await page.eval("document.querySelector('.approval-button.is-allow').click(); true");
  await idle(page);
  await waitFor('the approval card to go', () => page.eval("!document.querySelector('.approval')"));
  const reply = await lastReply(page);
  assert.match(reply, /Scripted reply to: hello approve browser/);
  assert.match(reply, /Approval: allow/);
  assert.match(reply, /Browser: Tabs:/);
  assert.equal(await page.eval("!!document.querySelector('.message.is-assistant .message-tools')"), true);
 });
 await step('usage is counted and the backend names the chat', async () => {
  await waitFor('the usage', () => page.eval("Usage.totals(1).test?.tokens === 150"));
  await waitFor('the title', () => page.eval("library.chats[0].title === 'Scripted title'"));
  assert.equal(await page.eval('Math.round(chat.fill * 100)'), 15);
 });
 await step('Escape stops a reply', async () => {
  await send(page, 'please hang');
  await waitFor('the reply to start', () => page.eval("chat.busy && /please hang/.test(document.querySelectorAll('.message.is-assistant')[document.querySelectorAll('.message.is-assistant').length - 1].textContent)"));
  await page.eval("document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })); true");
  await idle(page);
  await waitFor('the note', async () => /Stopped/.test(await lastOf(page, '.message-note')));
 });
 await step('an auth error shows with Open settings and Retry', async () => {
  await send(page, 'fail please');
  await idle(page);
  await waitFor('the error', async () => await lastOf(page, '.message-error') === 'Test provider rejected the key');
  assert.equal(await lastOf(page, '.message-actions'), 'Open settingsRetry');
 });
 await step('a manual compaction shows its line', async () => {
  assert.equal(await page.eval('chat.canCompact'), true);
  await page.eval('chat.compactNow()');
  await idle(page);
  await waitFor('the compaction line', () => page.eval("[...document.querySelectorAll('.thread-compact-text')].at(-1)?.textContent === 'Conversation compacted'"));
 });
 await step('the chat is saved for the display only and opens again', async () => {
  const saved = await page.eval('library.conversation(chat.activeId)');
  const roles = saved.messages.map(entry => entry.role);
  assert.ok(roles.includes('user') && roles.includes('assistant') && roles.includes('compact'));
  assert.equal(saved.messages.some(entry => entry.steps || entry.native || Array.isArray(entry.content)), false);
 });
 await step('a backend that crashes mid-reply is reported', async () => {
  await send(page, 'crash now');
  await idle(page);
  await waitFor('the error', async () => /backend stopped/i.test(await lastOf(page, '.message-error'))).catch(async error => {
   throw new Error(`${error.message}; state ${JSON.stringify(await page.eval("({ state: Backend.state, process: Backend.process, busy: chat.busy, errors: [...document.querySelectorAll('.message-error')].map(e => e.textContent), notes: [...document.querySelectorAll('.message-note')].map(e => e.textContent) })"))}`);
  });
  assert.equal(await page.eval('Backend.available'), false);
 });
 await step('no page errors with the scripted backend', async () => assert.deepEqual(page.errors, []));
 await app.stop();
}

const failed = results.filter(([state]) => state === 'FAIL').length;
console.log(`\n${results.length - failed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
