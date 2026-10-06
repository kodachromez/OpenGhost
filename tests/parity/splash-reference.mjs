// OpenGhost 1.3's own splash (reference/openghost/index.html) on a stepped
// clock, in one strictly headless browser: performance.now, animation frames,
// timers and Web Animations all follow a virtual time advanced a frame every
// 1/240 s from a fixed start, exactly as `openghost-cpp --splash-at` steps the
// native splash, and the page is captured at each requested scene time.
// usage: node splash-reference.mjs <output> <width> <height> <dpr> <ms,ms,...>
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const [output, width, height, dpr, times] = process.argv.slice(2);
const wanted = times.split(',').map(Number).sort((a, b) => a - b);
const root = path.resolve(import.meta.dirname, '../..');
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'openghost-splash-browser-'));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
fs.mkdirSync(output, {recursive: true});

// Installed before any page script. START and FRAME match tests/splash_frames.cpp.
const SHIM = `(() => {
  const START = 1000, FRAME = 1000 / 240;
  let vt = START;
  const macrotask = (() => { const c = new MessageChannel(), q = [];
    c.port1.onmessage = () => q.shift()(); return () => new Promise(r => { q.push(r); c.port2.postMessage(0); }); })();
  performance.now = () => vt;
  let frames = new Map(), nextFrame = 1;
  window.requestAnimationFrame = cb => { frames.set(nextFrame, cb); return nextFrame++; };
  window.cancelAnimationFrame = id => frames.delete(id);
  const realTimeout = window.setTimeout.bind(window), realClear = window.clearTimeout.bind(window);
  let timers = new Map(), nextTimer = 1e6;
  window.setTimeout = (fn, ms = 0, ...args) => {
    if (!(ms > 0)) return realTimeout(fn, 0, ...args);
    const id = nextTimer++; timers.set(id, {due: vt + ms, fn, args}); return id;
  };
  window.clearTimeout = id => { if (!timers.delete(id)) realClear(id); };
  // ghost-thinking.js's poses and blinks: each ghost draws from its own
  // generator, seeded alike, as the native capture's Ghosts do
  // (GhostItem::setTestRandom). Nothing else on the page changes.
  const realRandom = Math.random, realDefine = customElements.define.bind(customElements);
  let ghost = null;
  const seeds = new WeakMap();
  Math.random = () => {
    if (!ghost) return realRandom();
    const seed = (Math.imul(seeds.has(ghost) ? seeds.get(ghost) : 7, 1664525) + 1013904223) >>> 0;
    seeds.set(ghost, seed);
    return seed / 4294967296;
  };
  customElements.define = (name, type, options) => {
    if (name === 'ghost-thinking')
      for (const method of ['connectedCallback', 'tick']) {
        const real = type.prototype[method];
        type.prototype[method] = function (...args) {
          const outer = ghost; ghost = this;
          try { return real.apply(this, args); } finally { ghost = outer; }
        };
      }
    return realDefine(name, type, options);
  };
  // Web Animations follow the virtual time from when each was made or seen.
  const made = new WeakMap();
  const realAnimate = Element.prototype.animate;
  Element.prototype.animate = function (...args) {
    const a = realAnimate.apply(this, args); made.set(a, vt); a.pause(); a.currentTime = 0; return a;
  };
  const sync = () => {
    for (const a of document.getAnimations()) {
      if (!made.has(a)) { made.set(a, vt); a.pause(); }
      if (a.playState === 'finished' || a.playState === 'idle') continue;
      const local = vt - made.get(a), end = a.effect.getComputedTiming().endTime;
      if (local >= end) a.finish(); else { a.pause(); a.currentTime = local; }
    }
  };
  const step = async at => {
    vt = at;
    for (const [id, t] of [...timers].sort((a, b) => a[1].due - b[1].due))
      if (t.due <= vt && timers.delete(id)) { t.fn(...t.args); await macrotask(); }
    const due = frames; frames = new Map();
    for (const cb of due.values()) cb(vt);
    await macrotask(); sync(); await macrotask(); sync();
  };
  // The scene's start (splash.js Stage.t0) is private; it is the frame the
  // mist is first drawn in, as ready() and draw() run in the same frame.
  // Each draw covers the whole canvas (no blending), so only the draw of the
  // frame being captured is made; the frames between only advance state.
  let t0 = 0, drawing = false;
  const realDraw = WebGLRenderingContext.prototype.drawArrays;
  WebGLRenderingContext.prototype.drawArrays = function (...args) { t0 ||= vt; if (drawing) return realDraw.apply(this, args); };
  // splash.js draws the mist at MIST.scale (.6) on a GPU and MIST.low on a
  // software renderer; the capture renders with SwiftShader in place of the
  // GPU the app runs on, so it reports a GPU and takes the GPU path.
  const realParameter = WebGLRenderingContext.prototype.getParameter;
  WebGLRenderingContext.prototype.getParameter = function (name) {
    return name === 0x9246 ? 'ANGLE (NVIDIA, Vulkan)' : realParameter.call(this, name);
  };
  window.__splash = {
    async to(t) {
      if (!t0) await step(vt);
      while (!t0) await step(vt + FRAME);
      const target = t0 + t;
      while (vt + FRAME < target - 1e-9) await step(vt + FRAME);
      drawing = true; await step(target); drawing = false;
      return vt - t0; },
  };
})();`;

const child = spawn(process.env.PARITY_BROWSER || '/opt/brave-bin/brave', [
  '--headless=new', '--ozone-platform=headless', '--remote-debugging-port=0',
  `--user-data-dir=${home}/profile`, '--no-first-run', '--no-default-browser-check',
  '--disable-background-networking', '--disable-component-update', '--disable-sync',
  '--disable-features=MediaRouter', '--password-store=basic', '--use-gl=angle',
  '--use-angle=swiftshader', '--enable-unsafe-swiftshader', `--force-device-scale-factor=${dpr}`, 'about:blank'
], {env: {...process.env, HOME: home, DISPLAY: '', WAYLAND_DISPLAY: '',
          XDG_CONFIG_HOME: `${home}/config`, XDG_CACHE_HOME: `${home}/cache`}, stdio: ['ignore', 'ignore', 'pipe']});
let diagnostics = '';
child.stderr.on('data', data => diagnostics = (diagnostics + data).slice(-1024 * 1024));
let ws;
try {
  let port;
  for (let i = 0; i < 300 && !port; i++) {
    try { port = fs.readFileSync(`${home}/profile/DevToolsActivePort`, 'utf8').split('\n')[0]; } catch {}
    if (child.exitCode !== null) throw Error(`Headless browser exited: ${diagnostics}`);
    if (!port) await sleep(50);
  }
  const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  ws = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let next = 1;
  const waiting = new Map(), errors = [];
  ws.onmessage = ({data}) => {
    const msg = JSON.parse(data);
    if (msg.id) { const call = waiting.get(msg.id); waiting.delete(msg.id); msg.error ? call.reject(Error(JSON.stringify(msg.error))) : call.resolve(msg.result); }
    if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails);
    if (msg.method === 'Fetch.requestPaused') void send('Fetch.failRequest', {requestId: msg.params.requestId, errorReason: 'BlockedByClient'});
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = next++; waiting.set(id, {resolve, reject}); ws.send(JSON.stringify({id, method, params}));
  });
  const evaluate = async expression => {
    const r = await Promise.race([send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true}),
      sleep(60000).then(() => { throw Error('Timed out: ' + expression); })]);
    if (r.exceptionDetails) throw Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  };
  await send('Runtime.enable'); await send('Page.enable');
  await send('Fetch.enable', {patterns: [{urlPattern: 'http://*'}, {urlPattern: 'https://*'}]});
  await send('Emulation.setDeviceMetricsOverride', {width: +width, height: +height, deviceScaleFactor: +dpr, mobile: false});
  await send('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'no-preference'}, {name: 'prefers-color-scheme', value: 'dark'}]});
  await send('Page.addScriptToEvaluateOnNewDocument', {source: `localStorage.clear(); window.openghost={desktop:true, platform:'linux'};
    let seed=7; Math.random=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/4294967296);`});
  await send('Page.addScriptToEvaluateOnNewDocument', {source: SHIM});
  await send('Page.navigate', {url: pathToFileURL(path.join(root, 'reference/openghost/index.html')).href});
  for (let i = 0; i < 400; i++) {
    if (await evaluate("document.readyState === 'complete' && !!window.SplashMist && window.SplashMist.done() && !!document.querySelector('.splash-word.is-set')")) break;
    if (i === 399) throw Error('Reference splash did not load');
    await sleep(25);
  }
  const shots = [];
  for (const t of wanted) {
    const at = await evaluate(`window.__splash.to(${t})`);
    const image = await send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
    const file = path.join(output, `reference-${Math.round(t)}.png`);
    fs.writeFileSync(file, Buffer.from(image.data, 'base64'));
    shots.push({t, at, file});
    console.log('captured', t, at);
  }
  fs.writeFileSync(path.join(output, 'reference.json'), JSON.stringify({shots, errors, version: await send('Browser.getVersion')}, null, 2));
  if (errors.length) throw Error(`Reference exceptions: ${JSON.stringify(errors)}`);
} finally {
  ws?.close(); child.kill('SIGTERM');
  await Promise.race([new Promise(resolve => child.once('exit', resolve)), sleep(3000)]);
  if (child.exitCode === null) child.kill('SIGKILL');
  fs.writeFileSync(path.join(output, 'browser.log'), diagnostics);
  fs.rmSync(home, {recursive: true, force: true});
}
