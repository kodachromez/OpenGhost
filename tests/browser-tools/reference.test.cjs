'use strict';

// Oracle/port-preparation tests, not a native browser implementation. Only the
// frozen OpenGhost reference is executed; no Electron, network, RPC or engine.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../../reference/openghost');
const schemas = require('./schemas.json');
const results = require('./results.json');
const cases = require('./cases.json');
const plain = value => JSON.parse(JSON.stringify(value));
const tick = () => new Promise(resolve => setImmediate(resolve));

function load({ available = true, domText = '' } = {}) {
  // Intentionally not an HTML parser: hand the reference converter a text-only
  // document. These cases qualify UTF-16 slicing/formatting, NOT DOM extraction.
  const body = {
    childNodes: [{ nodeType: 3, data: domText }], textContent: domText,
    querySelectorAll: () => [],
  };
  const context = vm.createContext({
    URL, AbortController, DOMException, setTimeout, clearTimeout, queueMicrotask,
    DOMParser: class { parseFromString() {
      return { title: '', body, querySelector: () => null, querySelectorAll: () => [] };
    } },
    openghost: available ? { browser: { run() {} } } : {},
    Backend: { handle() {}, on() {} },
  });
  context.window = context;
  for (const file of ['host-tools.js', 'chat.js'])
    vm.runInContext(fs.readFileSync(path.join(root, file), 'utf8'), context, { filename: file });
  return context;
}

test('exact eleven schemas, including descriptions and absence of extra constraints', () => {
  assert.deepEqual(plain(load().HostTools.schemas), schemas);
  assert.deepEqual(plain(load({ available: false }).HostTools.schemas), []);
  assert.equal(new Set(schemas.map(s => s.name)).size, 11);
  assert.deepEqual(cases.map(c => c.name), schemas.map(s => s.name));
  for (const c of cases) {
    assert.ok(results.some(r => r.id === c.result), c.name);
    const schema = schemas.find(s => s.name === c.name).parameters;
    for (const key of schema.required) assert.ok(key in c.args, `${c.name}.${key}`);
    assert.ok(c.blocked.length > 0);
  }
});

for (const fixture of results) test(`formatter: ${fixture.id}`, () => {
  const { HostTools } = load(fixture);
  assert.deepEqual(plain(HostTools.result(fixture.args, fixture.answer)), fixture.expected);
});

test('formatter precedence and metadata presence do not depend on truthiness', () => {
  const { HostTools } = load();
  const data = Object.fromEntries([
    'code', 'tabId', 'pageId', 'readId', 'refs', 'tabs', 'truncated', 'coverage',
    'scroll', 'width', 'height', 'scale', 'pageWidth', 'pageHeight', 'downloads',
  ].map(key => [key, null]));
  data.width = 0;
  data.truncated = false;
  data.tabs = [];
  assert.deepEqual(plain(HostTools.result({}, { ...data, text: '' })).data, data);
  const image = { text: 'image first', image: 'opaque', html: '<ignored>' };
  assert.deepEqual(plain(HostTools.result({}, image)), {
    content: [{ type: 'text', text: 'image first' },
      { type: 'image', dataUrl: 'opaque', label: 'Screenshot of the built-in browser' }], data: {},
  });
  assert.deepEqual(plain(HostTools.result({}, { ...image, error: 'failed' })), {
    isError: true, content: [{ type: 'text', text: 'Error: failed' }], data: { code: 'browser_error' },
  });
});

test('read boundary is 40,000 UTF-16 units, even through a surrogate pair; exact continuation notice', () => {
  const domText = 'x'.repeat(39999) + '😀Z';
  const { HostTools } = load({ domText });
  const answer = { html: '<fixture>', url: 'about:blank', tabId: 'T', pageId: 'P', readId: 'R' };
  const first = plain(HostTools.result({}, answer));
  assert.deepEqual(first.data, {
    tabId: 'T', pageId: 'P', readId: 'R', start: 0, end: 40000, total: 40002,
    hasMore: true, offsetUnit: 'utf16', sourceTruncated: false, truncated: true,
    coverage: 'html-derived; excludes form controls, shadow roots and iframe content; may include hidden text',
  });
  assert.equal(first.content[0].text, 'about:blank\n\n' + domText.slice(0, 40000)
    + '\n\n[Characters 0–40000 of 40002. Call browser_read with tabId="T", readId="R", start=40000 to read further.]');
  const next = plain(HostTools.result({ start: first.data.end }, answer));
  assert.equal(next.content[0].text, 'about:blank\n\n' + domText.slice(40000));
  assert.equal(next.data.hasMore, false);
  assert.equal(next.data.end, 40002);
  const beyond = plain(HostTools.result({ start: 50000 }, answer));
  assert.equal(beyond.content[0].text, 'about:blank\n\n(empty page)');
  assert.equal(beyond.data.start, 50000);
  assert.equal(beyond.data.end, 50000); // not clamped to total by reference
});

// Chat hand-back is outside the engine: execute its real owner logic against a
// passive panel. This does not implement a panel/queue or simulate tool effects.
function handoff(answer = { text: 'After hand-back', tabId: 'user-tab', pageId: 'fresh' }) {
  const context = load();
  let release;
  const runs = [];
  const panel = context.browserPanel = {
    userHas: true, drive() {}, cancel() {},
    waitForAgent: () => new Promise(resolve => { release = resolve; }),
    run: async (name, args) => { runs.push({ name, args }); return answer; },
  };
  const chat = Object.assign(Object.create(context.Chat.prototype), { showGhost() {} });
  const turn = {
    remote: 'turn', controller: new AbortController(), requests: new Set(),
    releases: new Set(), steps: new Set(), part: { view: {} },
  };
  const conv = { id: 'chat', turn };
  return { chat, conv, turn, runs, back() { panel.userHas = false; release(); } };
}

for (const c of cases) {
  test(`${c.name}: hand-back substitutes snapshot, never original arguments or target`, async () => {
    const h = handoff();
    const result = h.chat.onHostTool(h.conv, {
      sessionId: 'chat', turnId: 'turn', toolCallId: c.name, name: c.name, args: c.args,
    }, new AbortController().signal);
    await tick();
    assert.equal(h.runs.length, 0);
    h.back();
    assert.deepEqual(plain(await result), {
      content: [{ type: 'text', text: 'After hand-back' }],
      data: { tabId: 'user-tab', pageId: 'fresh' }, status: 'handed-back',
    });
    assert.deepEqual(plain(h.runs), [{ name: 'browser_snapshot', args: {} }]);
    assert.equal(h.turn.releases.size, 0);
    assert.equal(h.turn.steps.size, 0);
  });
}

test('failed hand-back snapshot retains handed-back status and tool error', async () => {
  const h = handoff({ error: 'This browser tab was closed', code: 'tab_gone' });
  const result = h.chat.onHostTool(h.conv, {
    sessionId: 'chat', turnId: 'turn', toolCallId: 'call', name: 'browser_screenshot', args: {},
  }, new AbortController().signal);
  await tick();
  h.back();
  assert.deepEqual(plain(await result), {
    content: [{ type: 'text', text: 'Error: This browser tab was closed' }],
    data: { code: 'tab_gone' }, isError: true, status: 'handed-back',
  });
  assert.deepEqual(plain(h.runs), [{ name: 'browser_snapshot', args: {} }]);
});

for (const reason of ['abort', 'message']) test(`hand-back released by ${reason} dispatches nothing`, async () => {
  const h = handoff(), controller = new AbortController();
  const result = h.chat.onHostTool(h.conv, {
    sessionId: 'chat', turnId: 'turn', toolCallId: 'call', name: 'browser_click', args: { pageId: 'P', ref: 1 },
  }, controller.signal);
  await tick();
  if (reason === 'abort') controller.abort();
  else for (const release of h.turn.releases) release('message');
  assert.deepEqual(plain(await result), {
    status: 'cancelled', ...(reason === 'message' ? { reason } : {}), content: [],
  });
  h.back();
  await tick();
  assert.equal(h.runs.length, 0);
  assert.equal(h.turn.releases.size, 0);
});
