'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');

// Only the settings rows are modeled; markup is kept separately to check escaping and label IDs.
class Node {
 constructor(className = '') {
  this.className = className;
  this.children = [];
  this.dataset = {};
  this.listeners = {};
  this.value = '';
  this.textContent = '';
  this.classList = { toggle() {}, add() {}, remove() {} };
 }
 addEventListener(name, fn) { this.listeners[name] = fn; }
 setAttribute() {}
 querySelectorAll(selector) {
  const selectors = selector.split(',').map(s => s.trim());
  const matches = node => selectors.some(s => {
   const name = s.match(/^\.([\w-]+)/)?.[1];
   return name && node.className.split(' ').includes(name) && (!s.includes('[data-provider]') || Object.hasOwn(node.dataset, 'provider'));
  });
  const nodes = [];
  const walk = parent => { for (const child of parent.children) { if (matches(child)) nodes.push(child); walk(child); } };
  walk(this);
  return nodes;
 }
 querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
}

class List extends Node {
 set innerHTML(html) {
  this.html = html;
  this.children = [...html.matchAll(/<section\b[^>]*>([\s\S]*?)<\/section>/g)].map(([, body]) => {
   const section = new Node('provider');
   section.children.push(new Node('provider-state'), new Node('provider-models'));
   for (const row of body.split('<div class="settings-row">').slice(1)) {
    if (row.includes('class="settings-key"')) {
     const input = new Node('settings-key');
     input.nextElementSibling = new Node('settings-key-eye');
     input.parentElement = new Node('settings-key-box');
     section.children.push(input, new Node('settings-status'));
    } else if (row.includes('settings-account"')) {
     const account = new Node('settings-account');
     account.children.push(new Node('settings-account-who'), new Node('settings-status'));
     section.children.push(account);
    }
   }
   return section;
  });
 }
}

const provider = (id = 'p', status = { connected: false }) => ({
 id, name: `Provider ${id}`, methods: [{ type: 'apiKey' }, { type: 'oauth' }], status,
});

async function page(catalog = [provider()]) {
 const listeners = new Map(), requests = [], timers = new Map();
 let timerId = 0;
 const Backend = {
  available: false, can: () => true, explain: error => ({ message: error.message }),
  on: (name, fn) => listeners.set(name, fn),
  emit: (name, params) => listeners.get(name)?.(params),
  request(method, params) {
   const request = { method, params };
   requests.push(request);
   if (method === 'models.list') return Promise.resolve(Backend.models);
   return new Promise((resolve, reject) => Object.assign(request, { resolve, reject }));
  },
  models: [{ provider: catalog[0]?.id || 'p', id: 'dynamic-model', name: 'Dynamic model' }],
 };
 const window = {
  Backend, URL, console,
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  I18n: { lang: 'en', t: key => key }, Glyphs: { eye: '<svg></svg>' },
  setTimeout: fn => { const id = ++timerId; timers.set(id, fn); return id; },
  clearTimeout: id => timers.delete(id),
 };
 window.window = window;
 const context = vm.createContext(window);
 for (const file of ['settings.js', 'settings-usage.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), context, { filename: file });
 window.Settings.prototype.pager = () => {};
 const list = new List(), dialog = new Node();
 dialog.querySelector = () => list;
 const settings = new window.Settings(dialog);
 Backend.available = true;
 const next = method => {
  const request = requests.find(item => item.method === method && !item.taken);
  assert.ok(request, `expected ${method}`);
  request.taken = true;
  return request;
 };
 const initial = settings.refreshAll();
 next('auth.providers').resolve(catalog);
 await initial;
 const type = (id, key) => { settings.inputs[id].value = key; settings.onKeyInput(id); };
 const debounce = () => { const work = [...timers.values()]; timers.clear(); for (const fn of work) fn(); };
 const statusText = id => Array.from(settings.statuses[id], node => node.textContent);
 return { settings, Backend, list, requests, next, type, debounce, statusText, window };
}

test('overlapping key saves: latest success wins, including repeated key text; late errors are ignored', async () => {
 const { settings, next, type, debounce, statusText } = await page();
 type('p', 'same'); debounce();
 const first = next('auth.setKey');
 type('p', 'other'); debounce();
 const second = next('auth.setKey');
 type('p', 'same'); debounce();
 const third = next('auth.setKey');
 assert.equal(third.params.key, 'same');
 third.resolve({ connected: true, keySaved: true });
 await tick();
 first.resolve({ connected: false, error: 'old key' });
 second.reject(new Error('old transport error'));
 await tick();
 assert.equal(settings.connected('p'), true);
 assert.equal(settings.status.p.keySaved, true);
 assert.equal(settings.inputs.p.value, '');
 assert.equal(settings.typed.p, '');
 assert.deepEqual(statusText('p'), ['', '']);
});

test('typing invalidates an old key save before the next debounce fires; deleting still sends null', async () => {
 const { settings, next, type, debounce, statusText } = await page();
 type('p', 'old'); debounce();
 const first = next('auth.setKey');
 type('p', 'new');
 first.resolve({ connected: true, keySaved: true });
 await tick();
 assert.equal(settings.connected('p'), false);
 assert.equal(settings.inputs.p.value, 'new');
 assert.deepEqual(statusText('p'), ['settings.key.checking', 'settings.key.checking']);
 type('p', ''); debounce();
 const removal = next('auth.setKey');
 assert.equal(removal.params.key, null);
 removal.resolve({ connected: false, keySaved: false });
 await tick();
 assert.equal(settings.status.p.keySaved, false);
});

test('login/logout and login/cancel races ignore late success and failure without clearing newer errors', async () => {
 for (const method of ['auth.logout', 'auth.cancel']) for (const reject of [false, true]) {
  const { settings, next, statusText } = await page();
  const login = settings.login('p'), old = next('auth.login');
  assert.equal(settings.status.p.waiting, true);
  const stop = settings.auth(method, 'p');
  next(method).resolve({ connected: false, error: 'new status' });
  await stop;
  if (reject) old.reject(new Error('late login failure'));
  else old.resolve({ connected: true, account: { email: 'old@example.test' } });
  await login;
  assert.equal(settings.connected('p'), false);
  assert.equal(settings.status.p.waiting, false);
  assert.deepEqual(statusText('p'), ['new status', 'new status']);
 }
});

test('an old login failure cannot end a newer login waiting state', async () => {
 const { settings, next } = await page();
 const first = settings.login('p'), old = next('auth.login');
 const cancel = settings.auth('auth.cancel', 'p');
 next('auth.cancel').resolve({ connected: false });
 await cancel;
 const second = settings.login('p'), current = next('auth.login');
 old.reject(new Error('old failure'));
 await first;
 assert.equal(settings.status.p.waiting, true);
 current.resolve({ connected: true, account: { email: 'new@example.test', plan: 'pro' } });
 await second;
 assert.equal(settings.status.p.account.email, 'new@example.test');
 assert.equal(settings.accounts.p.dataset.state, 'connected');
});

test('provider refreshes are last-request-wins and cannot overwrite intervening auth mutations', async () => {
 const { settings, next, statusText } = await page();
 const before = settings.refreshAll(), old = next('auth.providers');
 const logout = settings.auth('auth.logout', 'p');
 next('auth.logout').resolve({ connected: false, error: 'new notice' });
 await logout;
 old.resolve([provider('p', { connected: true })]);
 await before;
 assert.equal(settings.connected('p'), false);
 assert.deepEqual(statusText('p'), ['new notice', 'new notice']);
 const first = settings.refreshAll(), stale = next('auth.providers');
 const second = settings.refreshAll();
 next('auth.providers').resolve([provider('new')]);
 await second;
 stale.resolve([provider('old')]);
 await first;
 assert.deepEqual([...settings.order], ['new']);
});

test('a provider snapshot started during login cannot roll back its completed result', async () => {
 const { settings, next } = await page();
 const login = settings.login('p');
 const refresh = settings.refreshAll(), snapshot = next('auth.providers');
 next('auth.login').resolve({ connected: true });
 await login;
 snapshot.resolve([provider('p', { connected: false })]);
 await refresh;
 assert.equal(settings.connected('p'), true);
});

test('stale auth.changed payloads only invalidate; earlier provider reads cannot overwrite the fresh snapshot', async () => {
 const { settings, Backend, next } = await page();
 const login = settings.login('p');
 next('auth.login').resolve({ connected: true });
 await login;
 const first = settings.refreshAll(), stale = next('auth.providers');
 Backend.emit('auth.changed', { provider: 'p', status: { connected: false } });
 assert.equal(settings.connected('p'), true, 'event snapshot is not blindly applied');
 const fresh = next('auth.providers');
 fresh.resolve([provider('p', { connected: true, account: { email: 'current@example.test' } }), provider('added')]);
 await tick();
 stale.resolve([provider('p', { connected: false })]);
 await first;
 assert.equal(settings.status.p.account.email, 'current@example.test');
 assert.deepEqual([...settings.order], ['p', 'added']);
});

test('late login events cannot resurrect a logged-out account', async () => {
 const { settings, Backend, next } = await page();
 const logout = settings.auth('auth.logout', 'p');
 next('auth.logout').resolve({ connected: false });
 await logout;
 Backend.emit('auth.changed', { provider: 'p', status: { connected: true, account: { email: 'old' } } });
 assert.equal(settings.connected('p'), false);
 next('auth.providers').resolve([provider('p', { connected: false })]);
 await tick();
 assert.equal(settings.connected('p'), false);
 assert.equal(settings.status.p.account.email, '');
});

test('auth.changed during a mutation re-reads after completion, preserving successful API key cleanup', async () => {
 const { settings, Backend, next, type, debounce } = await page();
 type('p', 'key'); debounce();
 const key = next('auth.setKey');
 Backend.emit('auth.changed', { provider: 'p', status: { connected: true } });
 next('auth.providers').resolve([provider('p', { connected: true, account: { email: 'stale' } })]);
 await tick();
 assert.equal(settings.connected('p'), false, 'no pre-mutation snapshot is applied');
 key.resolve({ connected: true, keySaved: true, account: { email: 'also stale' } });
 await tick();
 assert.equal(settings.connected('p'), false, 'event invalidated the RPC status');
 assert.equal(settings.inputs.p.value, '', 'successful latest save still clears the secret');
 next('auth.providers').resolve([provider('p', { connected: true, keySaved: true })]);
 await tick();
 assert.equal(settings.connected('p'), true);
 assert.equal(settings.status.p.account.email, '');
});

test('backend close and provider removal invalidate pending auth, catalog results and debounced keys', async () => {
 for (const close of [true, false]) {
  const { settings, Backend, next, type, debounce, requests } = await page();
  const login = settings.login('p'), oldLogin = next('auth.login');
  type('p', 'never sent');
  const refresh = settings.refreshAll(), oldCatalog = next('auth.providers');
  if (close) {
   Backend.available = false;
   Backend.emit('closed');
   oldCatalog.resolve([provider()]);
  } else oldCatalog.resolve([]);
  await refresh;
  oldLogin.resolve({ connected: true });
  await login;
  debounce();
  assert.equal(settings.providers.length, 0);
  assert.equal(settings.connected('p'), false);
  assert.equal(requests.filter(item => item.method === 'auth.setKey').length, 0);
 }
});

test('arbitrary provider IDs round-trip without prototypes, DOM ID collisions or markup injection', async () => {
 const ids = ['__proto__', 'constructor', 'toString', 'a|b', 'space : ["<&>\n\0', '😀', ''];
 const catalog = ids.map(id => ({ ...provider(id), name: '<img src=x onerror=alert(1)>', group: '<group>' }));
 const { settings, next, list, window } = await page(catalog);
 assert.deepEqual([...settings.order], ids);
 assert.equal(Object.getPrototypeOf(settings.inputs), null);
 assert.equal(Object.getPrototypeOf(settings.status), null);
 assert.doesNotMatch(list.html, /<img|<group>/);
 const domIds = [...list.html.matchAll(/\bid="([^"]*)"/g)].map(match => match[1]);
 assert.equal(new Set(domIds).size, domIds.length);
 assert.ok(domIds.every(id => !/\s/.test(id)));
 for (const id of ids) {
  assert.equal(settings.inputs[id].dataset.provider, id);
  assert.equal(settings.accounts[id].dataset.provider, id);
  assert.equal(settings.statuses[id].length, 2, 'both method rows receive status');
  assert.equal(settings.groupOf(id), '<group>');
  const saved = settings.auth('auth.setKey', id, { key: 'secret' });
  const request = next('auth.setKey');
  assert.equal(request.params.provider, id);
  request.resolve({ connected: true, keySaved: true });
  await saved;
  assert.equal(settings.connected(id), true);
 }
 assert.equal(settings.models[0].name, 'Dynamic model');
 // Usage's generic color lookup must not interpret reserved names as inherited colors.
 const usage = Object.assign(Object.create(window.UsageSettings.prototype), { settings, limits: Object.create(null) });
 for (const id of ids) {
  const html = usage.frame(id, '');
  assert.match(html, /usage-provider t-turquoise/);
  assert.doesNotMatch(html, /<img|\[object Object\]|function Object/);
 }
});

test('malformed methods/status fields fail safely; duplicate and unsupported methods do not collide', async () => {
 const catalog = [
  null, [], { id: 12 },
  { id: 'bad', name: {}, group: [], methods: {}, status: { connected: 'yes' } },
  { ...provider('p'), name: [], methods: [null, false, {}, { type: 'deviceCode' }, { type: 'apiKey', label: {}, placeholder: [] }, { type: 'apiKey' }, { type: 'oauth', action: {} }, { type: 'oauth' }], status: { connected: true, waiting: 'yes', account: { email: {}, plan: 42 } } },
  provider('p'),
 ];
 const { settings, Backend, next, statusText, requests } = await page(catalog);
 assert.deepEqual([...settings.order], ['bad', 'p']);
 assert.equal(settings.nameOf('bad'), 'bad');
 assert.equal(settings.connected('bad'), false);
 assert.equal(settings.providers[0].methods.length, 0);
 assert.equal(settings.providers[1].methods.length, 2);
 assert.equal(settings.status.p.account.plan, '');
 assert.equal(settings.status.p.waiting, false);
 assert.doesNotThrow(() => settings.paint());
 for (const malformed of [null, [], 'connected', {}, { connected: 'false' }, { connected: true, error: { message: {} } }, { connected: true, error: 42 }]) {
  const auth = settings.auth('auth.login', 'p');
  next('auth.login').resolve(malformed);
  assert.equal(await auth, null);
  assert.equal(settings.connected('p'), true, 'invalid replies do not replace known state');
  assert.deepEqual(statusText('p'), ['Invalid provider status from backend.', 'Invalid provider status from backend.']);
  const count = requests.length;
  Backend.emit('auth.changed', { provider: 'p', status: malformed });
  Backend.emit('auth.changed', null);
  assert.equal(requests.length, count, 'malformed notifications are ignored');
 }
});

test('a malformed key-save status is not treated as a successful save', async () => {
 const { settings, next, type, debounce, statusText } = await page();
 type('p', 'keep this key'); debounce();
 next('auth.setKey').resolve({ connected: true, error: {} });
 await tick();
 assert.equal(settings.inputs.p.value, 'keep this key');
 assert.equal(settings.connected('p'), false);
 assert.match(statusText('p')[0], /Invalid provider status/);
});

test('models.changed refreshes dynamic provider metadata as well as models', async () => {
 const { settings, Backend, next } = await page();
 Backend.models = [{ provider: 'new', id: 'new-model', name: { malformed: true } }];
 Backend.emit('models.changed', {});
 next('auth.providers').resolve([provider('new')]);
 await tick();
 assert.deepEqual([...settings.order], ['new']);
 assert.equal(settings.models[0].provider, 'new');
 assert.equal(settings.models[0].name, 'new-model');
});
