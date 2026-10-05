'use strict';

// Audit F05: exercise the real lock templates and copy without changing the cache-lock lifecycle.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT } = require('./helpers');

// Only the DOM surface needed to construct the screen and render either card mode.
class Element {
 constructor() {
  this.innerHTML = '';
  this.style = {};
  this.classList = { toggle() {}, remove() {} };
 }
 querySelector(selector) {
  if (selector === '.pass-input' && !this.innerHTML.includes('<input')) return null;
  return new Element();
 }
 get firstElementChild() { return new Element(); }
 getAnimations() { return []; }
 setAttribute() {}
 addEventListener() {}
 append() {}
 after() {}
}

function setup(encrypted) {
 const window = vm.createContext({
  document: { createElement: () => new Element(), addEventListener() {} },
  StageWord: class {},
  Backend: {
   capabilities: { sessions: encrypted === undefined ? {} : { encrypted } },
   can: name => name === 'sessions.encrypted' && encrypted === true,
  },
 });
 window.window = window;
 for (const file of ['i18n.js', 'lock-ui.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 }
 return window;
}

function assertScope(html, className) {
 // The warning must be a persistent paragraph, not the transient password/error status.
 const note = html.match(new RegExp(`<p class="${className}">([^<]+)</p>`))?.[1];
 assert.ok(note, `missing ${className}`);
 assert.match(note, /encrypts only this app’s display cache/);
 assert.match(note, /does not protect backend\/session history/);
 return note;
}

for (const encrypted of [undefined, false, true]) {
 test(`lock setup, management and screen disclose cache-only protection (sessions.encrypted: ${encrypted})`, () => {
  // A capability flag alone is not a password-protection lifecycle agreement.
  const window = setup(encrypted);
  for (const mode of ['set', 'guard']) {
   const card = Object.assign(Object.create(window.LockCard.prototype), {
    mode, root: new Element(), body: new Element(),
   });
   card.render();
   const note = assertScope(card.body.innerHTML, 'lock-card-note');
   if (mode === 'set') {
    assert.match(note, /forget the password, this encrypted cache can’t be opened/);
    assert.match(card.body.innerHTML, /type="password"/);
   } else {
    assert.match(card.body.innerHTML, /data-act="lock"/);
    assert.match(card.body.innerHTML, /data-act="remove"/);
   }
  }
  const screen = new window.LockScreen({ main: new Element() });
  assertScope(screen.root.innerHTML, 'lock-screen-note');
  assert.match(screen.root.innerHTML.replace(/<[^>]*>/g, ''), /This local view is locked/);
  assert.match(screen.root.innerHTML, /type="password"/);
 });
}

test('lock labels, encryption progress and forgotten-password copy are scoped to the local view/cache', () => {
 const { I18n } = setup();
 for (const key of ['chat.lock', 'chat.unlock', 'chat.protected', 'chat.locked', 'lock.set.title', 'lock.guard.title', 'lock.screen.title']) {
  assert.match(I18n.t(key), /local view/i, key);
 }
 assert.match(I18n.t('lock.set.text'), /this app’s cached chat view/);
 assert.match(I18n.t('lock.set.busy'), /Encrypting the display cache/);
 assert.match(I18n.t('lock.set.note'), /this encrypted cache can’t be opened/);
 assert.doesNotMatch(I18n.t('lock.set.note'), /chat can’t be recovered/);
});
