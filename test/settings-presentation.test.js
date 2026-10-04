'use strict';

// F19: local file measurements and generic presentation must not imply model/provider defaults.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');

function load(files, globals = {}) {
 const window = vm.createContext({ console, setTimeout, clearTimeout, ...globals });
 window.window = window;
 for (const file of ['i18n.js', ...files]) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 return window;
}

async function pinned() {
 const saved = new Map();
 const window = load(['file-kinds.js', 'user-context.js', 'settings-general.js'], {
  ChatStore: { read: async key => saved.get(key), write: async (key, value) => saved.set(key, value), remove: async key => saved.delete(key) },
  AttachmentReader: { read: async file => file.payload },
  Library: { samePath: (a, b) => a === b }, openghost: { pathOf: file => `/files/${file.name}` },
  SmoothHeight: class {},
 });
 await window.UserContext.ready;
 return window;
}

const file = (name, payload) => ({ name, size: 100, payload });
const textFile = (name, chars) => file(name, { type: 'text', text: 'a'.repeat(chars) });

test('pinned text metadata shows exact character counts, not estimated tokens; other metadata stays intact', async () => {
 const window = await pinned(), general = Object.create(window.GeneralSettings.prototype);
 for (const chars of [0, 3200, 200000]) {
  assert.equal(general.meta({ name: 'notes.txt', size: 100, kind: 'text', chars }), `Text · 100 B · ${chars.toLocaleString('en')} text characters`);
 }
 assert.equal(general.meta({ name: 'photo.png', size: 100, kind: 'image', width: 640, height: 480 }), 'Image · 100 B · 640×480');
 assert.equal(general.meta({ name: 'data.bin', size: 100, kind: 'none' }), 'App · 100 B · read from disk when needed');
});

test('local pinned-text cap counts only text; replacement and the separate file-count cap still work', async () => {
 const { UserContext: context } = await pinned();
 assert.equal((await context.add([
  textFile('notes.txt', 200000), file('photo.png', { type: 'image', url: 'data:image/png;base64,AA==' }), file('data.bin', { type: 'none' }),
 ])).length, 0, 'images and paths have no invented character/token weight');
 assert.equal(context.textChars(), 200000);
 assert.equal((await context.add([textFile('extra.txt', 1)]))[0].reason, 'size');
 assert.equal((await context.add([textFile('notes.txt', 199999), textFile('extra.txt', 1)])).length, 0);
 assert.equal(context.files.length, 4, 'the old copy is replaced rather than charged twice');
 assert.equal(context.textChars(), 200000);
 assert.equal(context.forBackend().files[0].text.length, 199999);
 const paths = Array.from({ length: 16 }, (_, i) => file(`${i}.bin`, { type: 'none' }));
 assert.equal((await context.add(paths)).length, 0);
 assert.equal((await context.add([file('overflow.bin', { type: 'none' })]))[0].reason, 'count');
});

test('pinned-file hint and rejection wording identify local limits, not model context limits', async () => {
 const window = await pinned(), context = window.UserContext;
 const node = () => ({ value: '', addEventListener() {}, querySelector: node });
 const root = node();
 const general = new window.GeneralSettings({ root, context: {
  limits: context.limits, files: [], instructions: '', onChange() {}, ready: Promise.resolve(),
 } });
 await tick();
 assert.match(root.innerHTML, /Local limits: 20 files and 200,000 extracted text characters, not model context limits/);
 general.context = context;
 general.say = text => { general.message = text; };
 await general.add([textFile('large.txt', 200001)]);
 assert.equal(general.message, 'Adding large.txt would exceed the local pinned-text limit of 200,000 characters. This is not a model context limit.');
 await general.add(Array.from({ length: 21 }, (_, i) => file(`${i}.bin`, { type: 'none' })));
 assert.equal(general.message, 'The local pinned-file limit is 20 files.');
});

test('usage applies one generic palette to every provider while preserving reported totals, cache and model names', async () => {
 const window = load(['usage.js', 'settings-usage.js'], {
  ChatStore: { read: async () => null, write: async () => {} },
  MutationObserver: class { observe() {} }, CSS: { escape: value => value },
 });
 const ids = ['deepseek', 'chatgpt', 'openai', 'anthropic', 'openai-codex', 'other', '__proto__'];
 await window.Usage.ready;
 for (const provider of ids) window.Usage.record({ provider, model: 'm', modelName: 'Backend model', input: 100, output: 20, cached: 25, requests: 2 });
 await tick();
 window.Usage.flush();
 const root = { addEventListener() {}, querySelector: () => null };
 const settings = { order: ids, providers: ids.map(id => ({ id })), connected: () => true, nameOf: id => `Provider ${id}` };
 const usage = new window.UsageSettings({ root, settings, dialog: {} });
 usage.render(false);
 const tones = ['turquoise', 'lilac', 'orange', 'blue'];
 ids.forEach((id, i) => {
  const tone = tones[i % tones.length];
  assert.ok(root.innerHTML.includes(`usage-provider t-${tone}" data-provider="${id}"`));
  assert.ok(root.innerHTML.includes(`usage-key t-${tone}"><i></i>Provider ${id}<b>120</b>`));
 });
 assert.match(root.innerHTML, /data-value="840" title="840 tokens"/);
 assert.match(root.innerHTML, /Input 100/);
 assert.match(root.innerHTML, /Output 20/);
 assert.match(root.innerHTML, /25% of it from cache/);
 assert.match(root.innerHTML, /2 requests/);
 assert.match(root.innerHTML, /Backend model/);
 assert.equal(usage.sums.at(-1), 840);
 const tip = {};
 root.querySelector = () => tip;
 usage.fillTip(usage.days.length - 1);
 assert.match(tip.innerHTML, /840 tokens/);
 assert.match(tip.innerHTML, /usage-tip-row t-orange.*Provider __proto__/);
});
