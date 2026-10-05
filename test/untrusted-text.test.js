'use strict';

// Text that comes from outside the app (a URL, a backend's error, a tool's name and arguments) is drawn as text, never
// parsed as HTML (audit F10). The page is a fake DOM that keeps every string handed to innerHTML, so a test can tell
// markup the app wrote itself from hostile text that reached the parser.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT } = require('./helpers');

const HOSTILE = ['<img src=x onerror=alert(1)>', '<script>alert(1)</script>', '"><b id="injected">x</b>'];
const MARKUP = /<img|<script|<b id|onerror/i;

class FakeElement {
 constructor(doc, tag) {
  this.doc = doc;
  this.tagName = tag.toUpperCase();
  this.children = [];
  this.attributes = {};
  this.className = '';
  this.style = { setProperty() {} };
  const classes = new Set();
  this.classList = {
   add: name => classes.add(name), remove: name => classes.delete(name), contains: name => classes.has(name),
   toggle: (name, on = !classes.has(name)) => { if (on) classes.add(name); else classes.delete(name); return on; },
  };
 }
 set innerHTML(html) { this.doc.html.push(html); this.children = [{ html }]; }
 set textContent(text) { this.children = text === '' ? [] : [String(text)]; }
 get textContent() { return this.children.map(child => typeof child === 'string' ? child : child.html ?? child.textContent).join(''); }
 get childElementCount() { return this.children.filter(child => child instanceof FakeElement).length; }
 get firstChild() { return this.children[0]; }
 append(...nodes) { this.children.push(...nodes); }
 prepend(...nodes) { this.children.unshift(...nodes); }
 setAttribute(name, value) { this.attributes[name] = String(value); }
 addEventListener() {}
 // Every element below this one, depth first.
 *walk() { for (const child of this.children) if (child instanceof FakeElement) { yield child; yield* child.walk(); } }
}

function page() {
 const doc = { html: [], activeElement: null };
 doc.createElement = tag => new FakeElement(doc, tag);
 const window = {
  console, setTimeout, clearTimeout, setImmediate, queueMicrotask, AbortController, DOMException, URL,
  document: doc, navigator: { language: 'en-US' },
  localStorage: { getItem: () => null, setItem() {} },
  matchMedia: () => ({ matches: true }),
  Glyphs: { terminal: '<svg data-glyph="terminal"></svg>', file: '<svg></svg>', folder: '<svg></svg>', globe: '<svg></svg>' },
 };
 window.window = window;
 const context = vm.createContext(window);
 for (const file of ['i18n.js', 'backend-protocol.js', 'backend-client.js', 'approval-card.js', 'browser-panel.js', 'chat.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), context, { filename: file });
 }
 return { window, doc };
}

// No markup but the app's own reached innerHTML.
function noHostileHtml(doc) {
 for (const html of doc.html) assert.doesNotMatch(html, MARKUP, `hostile text was parsed as HTML: ${html}`);
}

// The browser's address bar as syncBar draws it for the active tab's URL.
function addressBar(window, url) {
 const doc = window.document;
 const panel = Object.create(window.BrowserPanel.prototype);
 const button = () => doc.createElement('button');
 Object.assign(panel, {
  root: doc.createElement('aside'), url: { value: '' }, urlView: doc.createElement('div'),
  backButton: button(), forwardButton: button(), reloadButton: button(),
  active: { id: 1, url, view: { canGoBack: () => false, canGoForward: () => false } },
 });
 const before = doc.html.length;
 panel.syncBar();
 const parsed = doc.html.slice(before);
 assert.equal(parsed.length, 1, 'only the trusted reload/stop SVG may reach innerHTML, never URL text');
 assert.match(parsed[0], /^<svg\b/);
 return panel.urlView;
}

test('a hostile URL in the address bar is shown as text, in the same host and path pieces as before', () => {
 const { window, doc } = page();
 for (const hostile of HOSTILE) {
  const url = `https://example.invalid/${encodeURIComponent(hostile)}?q=${encodeURIComponent('<script>')}`;
  const view = addressBar(window, url);
  noHostileHtml(doc);
  const pieces = [...view.walk()].map(el => [el.className, el.textContent]);
  assert.deepEqual(pieces, [['browser-url-host', 'example.invalid'], ['browser-url-dim', decodeURI(new URL(url).pathname + new URL(url).search)]]);
  assert.match(view.textContent, /<(img|script|b)\b/);
 }
 // A URL that is not https keeps its scheme as a dim lead, as text too.
 const view = addressBar(window, 'http://example.invalid/%3Cimg%20src=x%20onerror=alert(1)%3E');
 noHostileHtml(doc);
 assert.deepEqual([...view.walk()].map(el => el.textContent), ['http://', 'example.invalid', '/<img src=x onerror=alert(1)>']);
});

test('issue #21: the encoded address-bar URL renders literal markup, not an element', () => {
 const { window } = page();
 const view = addressBar(window, 'https://example.invalid/%3Cb%20id=%22injected%22%3Ehello%3C/b%3E');
 assert.equal(view.textContent, 'example.invalid/<b id="injected">hello</b>');
 assert.deepEqual([...view.walk()].map(el => [el.tagName, el.className, el.textContent]), [
  ['SPAN', 'browser-url-host', 'example.invalid'],
  ['SPAN', 'browser-url-dim', '/<b id="injected">hello</b>'],
 ]);
});

test('address-bar protocol, host, path, query and hash retain text-only formatting', () => {
 const { window } = page();
 const markup = '%3Cb%20id=%22injected%22%3Ehello%3C/b%3E';
 const cases = [
  ['http://www.example.invalid:8080/', ['http://', 'example.invalid:8080', '']],
  [`https://example.invalid/?q=${markup}`, ['example.invalid', '?q=<b id="injected">hello</b>']],
  [`https://example.invalid/#${markup}`, ['example.invalid', '#<b id="injected">hello</b>']],
  ['https://example.invalid/&lt;b&gt;%20%E2%9C%93', ['example.invalid', '/&lt;b&gt; ✓']],
  [`file:///tmp/${markup}`, ['file://', '', '/tmp/<b id="injected">hello</b>']],
  [`data:text/html,${markup}`, ['data://', '', 'text/html,<b id="injected">hello</b>']],
 ];
 for (const [url, pieces] of cases) {
  const view = addressBar(window, url);
  assert.deepEqual([...view.walk()].map(el => el.textContent), pieces, url);
  assert.ok([...view.walk()].every(el => el.tagName === 'SPAN' && el.childElementCount === 0), url);
 }
});

test('invalid URLs and malformed escapes fall back to literal text; blank URLs stay empty', () => {
 const { window } = page();
 for (const url of ['not a URL <b id="injected">hello</b>', 'http://example.invalid/%ZZ%3Cb%3E']) {
  const view = addressBar(window, url);
  assert.equal(view.textContent, url);
  assert.equal(view.childElementCount, 0, 'fallback must replace any partial protocol/host display');
 }
 for (const url of ['', 'about:blank']) {
  const view = addressBar(window, url);
  assert.equal(view.textContent, '');
  assert.equal(view.childElementCount, 0);
 }
});

test('an approval card shows a hostile tool name, arguments and presentation as text', () => {
 const { window, doc } = page();
 const { ApprovalCard } = window;
 const bare = new ApprovalCard(ApprovalCard.present(null, HOSTILE[0], { command: HOSTILE[1], note: HOSTILE[2] }));
 const presented = new ApprovalCard(ApprovalCard.present({
  kind: 'file', title: HOSTILE[1], effect: 'change', badge: true, quote: HOSTILE[0], reveal: 'changes',
  places: [{ kind: 'file', label: HOSTILE[0], title: HOSTILE[2] }], removed: HOSTILE[1], added: HOSTILE[2],
 }, 'edit', {}));
 noHostileHtml(doc);
 assert.ok(bare.el.textContent.includes(HOSTILE[0]));
 assert.ok(bare.el.textContent.includes(JSON.stringify(HOSTILE[1])));
 for (const hostile of HOSTILE) assert.ok(presented.el.textContent.includes(hostile));
});

test('approval fallback does not amplify nested JSON with indentation or hide argument suffixes', () => {
 const { window } = page();
 let nested = { script: `echo ${'x'.repeat(8192)}; important-final-command` };
 for (let i = 0; i < 500; i++) nested = { child: nested };
 const args = { nested, final: 'all arguments remain visible' }, original = JSON.stringify(args);
 const info = window.ApprovalCard.present(null, 'custom_tool', args);
 assert.equal(info.code, original, 'stock uses compact JSON; pretty printing adds quadratic whitespace');
 assert.equal(JSON.stringify(args), original, 'presentation must not alter backend arguments');
 const card = new window.ApprovalCard(info);
 assert.equal([...card.el.walk()].find(node => node.className === 'approval-code').textContent, original);
 assert.ok(card.el.textContent.includes('important-final-command'));
});

test('a hostile backend error and provider name are shown as text under the reply', () => {
 const { window, doc } = page();
 const chat = Object.assign(Object.create(window.Chat.prototype), { settings: { open() {} } });
 for (const error of [{ message: HOSTILE[0] }, { code: 'auth', provider: HOSTILE[1] }, { code: 'rate_limit', provider: HOSTILE[2] }]) {
  const view = { el: doc.createElement('div') };
  chat.fail({}, view, new window.BackendError(error));
  noHostileHtml(doc);
  const box = view.el.children.find(child => child.className === 'message-error');
  assert.ok(box.textContent.includes(error.message || error.provider), box.textContent);
 }
});
