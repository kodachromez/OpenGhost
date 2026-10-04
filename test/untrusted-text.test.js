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
 for (const file of ['i18n.js', 'backend-client.js', 'approval-card.js', 'browser-panel.js', 'chat.js']) {
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
 panel.syncBar();
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
