'use strict';

// Loads the app's classic renderer scripts (each an IIFE that sets window.*) into a fresh context, without a browser.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ROOT = path.join(__dirname, '..');

function renderer(files, globals = {}) {
 const window = { ...globals };
 const context = vm.createContext({
  window, console, setTimeout, clearTimeout, setImmediate, queueMicrotask, Promise, DOMException, AbortController,
  navigator: { language: 'en-US' }, structuredClone,
 });
 window.window = window;
 for (const file of files) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), context, { filename: file });
 return window;
}

// A transport like the preload bridge's: what the client sends is kept, and the test plays the backend.
function fakeTransport(state = 'running') {
 const sent = [];
 let onMessage = () => {}, onStatus = () => {};
 return {
  sent,
  send: message => sent.push(JSON.parse(JSON.stringify(message))),
  onMessage: callback => { onMessage = callback; },
  onStatus: callback => { onStatus = callback; },
  status: () => Promise.resolve({ state }),
  deliver: message => onMessage({ jsonrpc: '2.0', ...message }),
  setStatus: status => onStatus(status),
  last: () => sent[sent.length - 1],
 };
}

const tick = () => new Promise(resolve => setImmediate(resolve));

module.exports = { ROOT, renderer, fakeTransport, tick };
