'use strict';

// Settings → Usage account limits on the generic account.limits contract (AccountLimits | null).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');

// Just enough DOM for one provider section: its header, the account slot and an optional limits box.
function element(className) {
 const node = { className, innerHTML: '', hidden: false, textContent: '', children: [], classList: { toggle() {} } };
 node.after = sibling => { node.parent.children.splice(node.parent.children.indexOf(node) + 1, 0, sibling); sibling.parent = node.parent; };
 node.querySelector = selector => {
  const name = selector.match(/^\.([\w-]+)$/)?.[1];
  return node.children.find(child => child.className === name) || null;
 };
 return node;
}

function page({ box = true, catalogLimits = true } = {}) {
 const section = element('usage-provider');
 const head = element('provider-head'), slot = element('usage-account');
 slot.hidden = true;
 slot.children.push(element('usage-account-label'), element('usage-account-value'));
 section.children.push(head);
 head.parent = section;
 if (box) { const limits = element('usage-limits'); limits.parent = section; section.children.push(limits); }
 const root = {
  addEventListener() {},
  querySelector(selector) {
   if (selector === '.usage-provider[data-provider="p"]') return section;
   if (selector === '.usage-provider[data-provider="p"] [data-slot="account"]') return slot;
   return null;
  },
 };
 const requests = [];
 const window = vm.createContext({
  console, setTimeout, clearTimeout, CSS: { escape: value => value },
  MutationObserver: class { observe() {} },
  ChatStore: { read: async () => null, write: async () => {} },
  document: { createElement: () => element('') },
  matchMedia: () => ({ matches: true }),
  Backend: {
   can: name => name === 'usage.limits',
   request: (method, params) => new Promise((resolve, reject) => requests.push({ method, params, resolve, reject })),
  },
 });
 window.window = window;
 for (const file of ['i18n.js', 'usage.js', 'settings-usage.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 const state = { connected: true };
 const settings = { order: ['p'], providers: [{ id: 'p', limits: catalogLimits }], connected: () => state.connected, nameOf: id => id };
 const usage = new window.UsageSettings({ root, settings, dialog: {} });
 usage.visible = true;
 const limitsBox = () => section.querySelector('.usage-limits');
 return { usage, state, requests, section, slot, limitsBox };
}

const WAITING = /is-waiting/;

test('a null account.limits reply settles the box instead of leaving the loading placeholder, also after a sign-out', async () => {
 for (const signOut of [false, true]) {
  const { usage, state, requests, limitsBox } = page();
  usage.refresh();
  usage.paintLimits('p', false);
  assert.match(limitsBox().innerHTML, WAITING);
  if (signOut) state.connected = false;
  requests[0].resolve(null);
  await tick();
  assert.doesNotMatch(limitsBox().innerHTML, WAITING);
  assert.match(limitsBox().innerHTML, /This plan has no usage limits/);
  assert.equal(usage.limits.p.state, 'ready');
 }
});

test('a plan kept from before a sign-out is not shown for the disconnected provider', async () => {
 const { usage, state, requests, slot } = page();
 usage.refresh();
 requests[0].resolve({ plan: 'pro', windows: [] });
 await tick();
 assert.equal(slot.hidden, false, 'a connected account shows its plan');
 slot.hidden = true;
 state.connected = false;
 usage.paintLimits('p', false);
 assert.equal(slot.hidden, true, 'the retained plan is not painted on a signed-out provider');
});

test('limits arriving for a section drawn without a limits box get a box; a signed-out provider does not', async () => {
 for (const connected of [true, false]) {
  const { usage, state, requests, section, limitsBox } = page({ box: false, catalogLimits: false });
  usage.refresh();
  state.connected = connected;
  requests[0].resolve({ windows: [{ seconds: 18000, used: 40, resets: 0 }] });
  await tick();
  if (!connected) { assert.equal(limitsBox(), null); continue; }
  assert.equal(section.children[1], limitsBox(), 'the box sits right under the provider header');
  assert.match(limitsBox().innerHTML, /usage-limit[^-]/);
  assert.match(limitsBox().innerHTML, /40%/);
 }
});
