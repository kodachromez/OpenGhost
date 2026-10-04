'use strict';

// F13: catalog omissions are not capabilities; configuration responses, not requests, are authoritative.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT } = require('./helpers');
const plain = value => JSON.parse(JSON.stringify(value));

function node() {
 const queries = new Map();
 return {
  attributes: {}, dataset: {}, children: [], style: { setProperty() {} },
  classList: { add() {}, remove() {}, toggle() {} },
  setAttribute(key, value) { this.attributes[key] = String(value); },
  removeAttribute(key) { delete this.attributes[key]; },
  toggleAttribute(key, on) { if (on) this.setAttribute(key, ''); else this.removeAttribute(key); },
  querySelector(key) { if (!queries.has(key)) queries.set(key, node()); return queries.get(key); },
  querySelectorAll: () => [], addEventListener() {},
  append(...items) { this.children.push(...items); }, replaceChildren(...items) { this.children = items; },
  remove() {}, focus() {}, showPopover() {}, matches: () => false,
  setLevel(value) { this.level = value; }, setCount(value) { this.count = value; },
 };
}

async function page(models, saved = {}) {
 const storage = new Map(Object.entries(saved)), calls = [];
 const backend = {
  available: false, on() {}, handle() {}, can: () => false,
  async request(method, params) {
   calls.push({ method, params: params && plain(params) });
   if (method === 'models.list') return backend.models;
   if (method === 'session.configure') return backend.canonical;
   throw new Error(`Unexpected request: ${method}`);
  },
  models,
 };
 const window = vm.createContext({
  console, setTimeout, clearTimeout, AbortController, DOMException,
  navigator: { language: 'en' }, document: { createElement: node, addEventListener() {} },
  localStorage: { getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, String(value)), removeItem: key => storage.delete(key) },
  Backend: backend, UserContext: { ready: Promise.resolve(), forBackend: () => ({}) },
  matchMedia: () => ({ matches: true }), addEventListener() {},
  ResizeObserver: class { observe() {} }, LiquidGlass: class {}, EffortPaint: class {},
  EffortMorph: class { setLevels() {} }, EffortStage: class { static nameOf(value) { return value; } },
  Glyphs: { bars: '<svg></svg>' },
 });
 window.window = window;
 for (const file of ['i18n.js', 'settings.js', 'chat.js', 'effort-slider.js', 'model-stage.js', 'stats-card.js', 'add-menu.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 }
 // Replace provider-page layout only. Catalog loading, selection, slider and configuration methods are real.
 window.Settings.prototype.build = function() {};
 window.Settings.prototype.pager = function() {};
 const settings = new window.Settings(node());
 backend.available = true;
 await settings.refresh();
 settings.show(settings.resolve());
 settings.open = reason => { settings.opened = reason; };
 const conv = { id: 's1', record: { model: settings.resolve() }, sessionVersion: 'v1', reconciled: true, messages: [], tokens: 250, window: 0 };
 const chat = Object.assign(Object.create(window.Chat.prototype), {
  settings, active: conv, conversations: new Map([['s1', conv]]),
  library: { update(id, patch) { assert.equal(id, conv.id); Object.assign(conv.record, patch); } },
  onChange() { settings.show(chat.model); }, cwd: () => '/work',
  summarize(conv, turn, work) { this.switching = work(); },
 });
 const slider = new window.EffortSlider({ button: node(), panel: node(), settings });
 const stage = Object.assign(Object.create(window.ModelStage.prototype), {
  settings, chat, button: node(), root: node(), list: node(), state: 'closed',
  place() {}, middle() {}, mark(row) { this.marked = row; }, close() { this.state = 'closed'; },
 });
 settings.freshen = () => {};
 return { window, settings, storage, backend, calls, chat, conv, slider, stage };
}

const model = (id, extra = {}) => ({ id, provider: 'p', name: id, ...extra });
const thinking = { thinkingLevels: ['none', 'low', 'high'], defaultThinking: 'low' };

test('empty or missing thinkingLevels stays unsupported in settings, slider, and outgoing configuration', async () => {
 for (const capabilities of [{}, { thinkingLevels: [] }, { thinkingLevels: [], defaultThinking: 'high' }]) {
  const f = await page([model('m', capabilities)], { 'openghost.effort': 'high' });
  assert.deepEqual(plain(f.settings.config.efforts), []);
  assert.equal(f.settings.config.effort, undefined);
  assert.deepEqual(plain(f.slider.efforts), []);
  assert.equal(f.slider.button.hidden, true);
  f.slider.toggle();
  assert.equal(f.slider.opened, false);
  const params = plain(await f.chat.sessionParams(f.conv, { config: f.chat.config(f.conv) }));
  assert.equal(Object.hasOwn(params, 'thinking'), false);
 }
});

test('missing vision is not advertised; explicit true and false retain their picker metadata', async () => {
 const f = await page([model('unknown'), model('text', { vision: false }), model('vision', { vision: true })]);
 f.stage.build();
 for (let i = 0; i < f.settings.models.length; i++) {
  const entry = f.settings.models[i], supported = i === 2;
  assert.equal(f.settings.configFor(entry.id).vision, supported);
  assert.match(f.stage.rows[i].attributes['aria-label'], new RegExp(f.window.I18n.t(supported ? 'model.vision' : 'model.text')));
  if (!supported) assert.doesNotMatch(f.stage.rows[i].attributes['aria-label'], /Vision/i);
 }
});

test('missing contextWindow stays unknown in the picker, context hint and stats; reported windows still work', async () => {
 const f = await page([model('m')]);
 assert.equal(f.settings.windowOf('p:m'), 0);
 assert.equal(f.settings.windowOf('removed'), 0);
 assert.equal(f.chat.fill, null);
 f.stage.build();
 assert.doesNotMatch(f.stage.rows[0].attributes['aria-label'], /context|1M/i);
 const stats = await f.chat.stats();
 assert.equal(stats.context.window, 0);
 const card = f.window.StatsCard.build(stats);
 assert.match(card.innerHTML, />—<\/span>/);
 assert.doesNotMatch(card.innerHTML, /NaN|Infinity|1M|% full/);
 const hints = {};
 const menu = Object.assign(Object.create(f.window.AddMenu.prototype), { chat: f.chat, dock: { set: (id, value) => { hints[id] = value; } } });
 Object.defineProperty(f.chat, 'canCompact', { value: true });
 menu.sync();
 assert.equal(hints.compact.hint, f.window.I18n.t('add.compact'));
 f.conv.window = 1000;
 assert.equal(f.chat.fill, 0.25);
 f.conv.window = 0;
 f.backend.models = [model('m', { contextWindow: 500 })];
 await f.settings.refresh();
 assert.equal(f.chat.fill, 0.5);
 assert.equal((await f.chat.stats()).context.window, 500);
});

test('backend defaults drive initial selection and same-level model switches without becoming saved preferences', async () => {
 const f = await page([model('a', thinking), model('b', { ...thinking, defaultThinking: 'none' })]);
 assert.equal(f.settings.config.effort, 'low');
 assert.equal(f.slider.value, 1);
 assert.equal(f.slider.button.level, 1);
 assert.equal(f.storage.has('openghost.effort'), false);
 f.chat.setModel('p:b');
 assert.equal(f.settings.config.effort, 'none');
 assert.equal(f.slider.value, 0);
 assert.equal(f.storage.has('openghost.effort'), false);
 f.slider.commit(2);
 assert.equal(f.settings.config.effort, 'high');
 f.chat.setModel('p:a');
 assert.equal(f.settings.config.effort, 'high');
 const saved = await page([model('a', thinking)], { 'openghost.effort': 'high' });
 assert.equal(saved.settings.config.effort, 'high');
});

test('historical effort preference migrates once to the neutral key without overriding a newer choice', async () => {
 for (const current of [undefined, 'none']) {
  const saved = { 'deepseek.effort': 'high', ...(current ? { 'openghost.effort': current } : {}) };
  const f = await page([model('m', thinking)], saved);
  assert.equal(f.settings.config.effort, current ?? 'high');
  assert.equal(f.storage.get('openghost.effort'), current ?? 'high');
  assert.equal(f.storage.has('deepseek.effort'), false);
  f.settings.setEffort(undefined);
  const reopened = await page([model('m', thinking)], Object.fromEntries(f.storage));
  assert.equal(reopened.settings.config.effort, 'low', 'clearing the preference must not revive the legacy choice');
  assert.equal(reopened.storage.has('openghost.effort'), false);
 }
});

test('advertised custom levels without a default are selectable but are not automatically sent or saved', async () => {
 const f = await page([model('m', { thinkingLevels: ['brief', 'deep'] })]);
 assert.equal(f.settings.config.effort, undefined);
 assert.equal(f.storage.has('openghost.effort'), false);
 assert.equal(f.slider.button.hidden, false);
 assert.equal(f.slider.button.attributes.label, f.window.I18n.t('effort'));
 f.slider.commit(1);
 assert.equal(f.settings.config.effort, 'deep');
 assert.equal(f.storage.get('openghost.effort'), 'deep');
 f.backend.models = [model('m', { thinkingLevels: [] })];
 await f.settings.refresh();
 assert.equal(f.settings.config.effort, undefined);
 assert.equal(f.slider.button.hidden, true);
});

test('model switching adopts backend canonical model/thinking/mode, including a cleared thinking value', async () => {
 for (const value of ['low', null]) {
  const f = await page(['a', 'b', 'canonical'].map(id => model(id, thinking)), { 'openghost.effort': 'high' });
  let modeSyncs = 0;
  f.window.ModePicker = { sync() { modeSyncs++; } };
  f.backend.canonical = { model: 'canonical', thinking: value, permissionMode: 'auto' };
  f.conv.messages.push({ role: 'user', content: 'history' });
  f.chat.switchModel('p:b');
  await f.chat.switching;
  const request = f.calls.find(call => call.method === 'session.configure');
  assert.equal(request.params.model, 'b');
  assert.equal(request.params.thinking, 'high');
  assert.equal(f.chat.model, 'p:canonical');
  assert.equal(f.conv.record.model, 'p:canonical');
  assert.equal(f.settings.model, 'p:canonical');
  assert.equal(f.conv.turn.config.id, 'p:canonical');
  assert.equal(f.conv.turn.config.effort, value ?? undefined);
  assert.equal(f.settings.config.effort, value ?? undefined);
  assert.equal(f.settings.mode, 'auto');
  assert.equal(modeSyncs, 1);
  const params = plain(await f.chat.sessionParams(f.conv, f.conv.turn));
  assert.equal(params.model, 'canonical');
  assert.equal(params.thinking, value ?? undefined);
  assert.equal(f.slider.button.attributes.label, value ? 'Effort: low' : 'Effort');
 }
});

test('canonical thinking is respected without turning it into an advertised capability', async () => {
 const f = await page([model('m', { thinkingLevels: [] })]);
 f.backend.canonical = { model: 'm', thinking: 'backend-managed' };
 const config = await f.chat.configure(f.conv, { model: 'm', provider: 'p' });
 assert.equal(config.effort, 'backend-managed');
 assert.deepEqual(plain(config.efforts), []);
 assert.equal(f.slider.button.hidden, true);
 f.backend.canonical = { permissionMode: 'ask' }; // Omitted fields do not overwrite known configuration.
 await f.chat.configure(f.conv, { permissionMode: 'ask' });
 assert.equal(f.chat.model, 'p:m');
 assert.equal(f.chat.config(f.conv).effort, 'backend-managed');
});

test('canonical provider and unavailable model are retained rather than replaced by a catalog choice', async () => {
 const f = await page([model('a', thinking), model('b', thinking)]);
 f.backend.canonical = { provider: 'other', model: 'not-listed', thinking: null };
 await f.chat.configure(f.conv, { model: 'b', provider: 'p' });
 assert.equal(f.chat.model, 'other:not-listed');
 assert.equal(f.settings.model, 'other:not-listed');
 assert.equal(f.settings.config.ready, false);
 assert.equal(f.slider.button.hidden, true);
 assert.equal(f.chat.send('do not switch providers'), false);
 assert.equal(f.settings.opened, f.window.I18n.t('settings.key.needed'));
});

test('removed selections stay unavailable; picker still supports explicit valid choices and legacy IDs', async () => {
 const f = await page([model('a', thinking), model('b', thinking)]);
 assert.equal(f.settings.resolve('a'), 'p:a');
 assert.equal(f.settings.resolve('p:b'), 'p:b');
 f.settings.setModel('p:a');
 f.backend.models = [model('b', thinking), model('p:a', { provider: 'other' })];
 await f.settings.refresh();
 assert.equal(f.chat.model, 'p:a');
 assert.equal(f.settings.resolve(), 'p:a');
 assert.equal(f.settings.config.ready, false);
 assert.equal(f.settings.config.vision, false);
 assert.deepEqual(plain(f.settings.efforts), []);
 assert.equal(f.chat.send('not sent'), false);
 assert.equal(f.calls.some(call => call.method === 'turn.start'), false);
 f.stage.sync();
 assert.match(f.stage.button.attributes.label, /unavailable/);
 f.stage.open();
 assert.equal(f.stage.current(), null);
 assert.equal(f.stage.rows[0].attributes['aria-selected'], 'false');
 f.stage.pick({ dataset: { model: 'p:a' } }); // Stale row after a catalog refresh.
 assert.equal(f.chat.model, 'p:a');
 f.stage.pick(f.stage.rows[0]);
 assert.equal(f.chat.model, 'p:b');
 assert.equal(f.settings.config.ready, true);
 assert.equal(f.stage.rows[0].attributes['aria-selected'], 'true');
 assert.equal(f.slider.button.hidden, false);
 f.backend.models = [];
 await f.settings.refresh();
 assert.equal(f.chat.model, 'p:b');
 assert.equal(f.settings.config.ready, false);
});

test('cached catalogs containing old inferred capabilities are not reused', async () => {
 const f = await page([model('m')]);
 f.storage.set('openghost.catalog', JSON.stringify({ version: 2, models: [{ id: 'p:old', vision: true, efforts: ['high'] }] }));
 assert.deepEqual(plain(f.settings.readCatalog()), []);
 f.settings.saveCatalog();
 assert.deepEqual(plain(f.settings.readCatalog()), plain(f.settings.models));
});

test('canonical thinking from a model switch is session state, not a replacement for the saved preference', async () => {
 const deep = { thinkingLevels: ['none', 'low', 'high', 'max'], defaultThinking: 'low' };
 const f = await page([model('deep', deep), model('plain')], { 'openghost.effort': 'max' });
 f.conv.messages.push({ role: 'user', content: 'history' });
 f.backend.canonical = { model: 'plain', thinking: null };
 f.chat.switchModel('p:plain');
 await f.chat.switching;
 assert.equal(f.calls.find(call => call.method === 'session.configure').params.thinking, undefined);
 assert.equal(f.settings.config.effort, undefined);
 assert.equal(f.slider.button.hidden, true);
 assert.equal(f.storage.get('openghost.effort'), 'max');
 assert.equal(f.settings.configFor('p:deep').effort, 'max');
});
