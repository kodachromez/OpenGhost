'use strict';

// approval.request and host.tool belong to the turn they name: a request from an older, stopped or other turn must not
// show a card or run a browser step in the turn that is running now (audit F01).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');

// chat.js names its globals bare (Backend, ApprovalCard, HostTools), so here the window is the context's global.
function load() {
 const window = vm.createContext({
  console, setTimeout, clearTimeout, setImmediate, queueMicrotask, Promise, DOMException, AbortController, structuredClone,
  navigator: { language: 'en-US' }, document: { readyState: 'complete' },
 });
 window.window = window;
 const cards = [];
 window.ApprovalCard = class {
  static present(presentation, tool) { return { tool }; }
  constructor(info) {
   this.info = info;
   this.el = {};
   this.answer = new Promise(resolve => { this.settle = resolve; });
   cards.push(this);
  }
  dismiss() {}
 };
 const ran = [], cancelled = [];
 window.browserPanel = {
  userHas: false,
  drive() {},
  run: (name, args, { id }) => { ran.push({ name, args, id }); return new Promise(resolve => { window.browserPanel.answer = resolve; }); },
  cancel: id => cancelled.push(id),
 };
 window.HostTools = { has: () => true, result: (args, answer) => ({ content: [{ type: 'text', text: answer?.text || '' }] }) };
 window.I18n = { t: key => key, has: () => true };
 for (const file of ['backend-protocol.js', 'backend-client.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 return { window, cards, ran, cancelled };
}

// Just enough of a Chat and its conversation for the turn bookkeeping: begin/drive/setRemote as the real ones run.
function chatWith(window) {
 const chat = Object.create(window.Chat.prototype);
 Object.assign(chat, {
  active: null, tools: 0, nodes: new Map(), library: { chat: () => null },
  dismissGhost() {}, showGhost() {}, followBottom() {}, onChange() {}, note() {}, fail() {},
  sessionOf: () => 'S1',
 });
 const conv = { id: 'c1', turn: null, messages: [], record: null };
 const start = () => {
  const turn = chat.begin(conv, { id: 'm' });
  turn.started = new Promise(resolve => { turn.onStarted = resolve; });
  const view = { el: { append() {}, classList: { remove() {} }, isConnected: true }, stream: { finish: async () => {} } };
  turn.part = { view, entry: { content: '' } };
  turn.parts.push(turn.part);
  return turn;
 };
 return { chat, conv, start };
}

const plain = value => JSON.parse(JSON.stringify(value));
const signal = () => new AbortController().signal;
const stale = error => error.code === 'stale_turn';

test('a host.tool naming another turn is refused and runs nothing in the running turn', async () => {
 const { window, ran } = load();
 const { chat, conv, start } = chatWith(window);
 chat.setRemote(conv, start(), 'T1');
 await assert.rejects(chat.onHostTool(conv, { sessionId: 'S1', turnId: 'DEFINITELY-OLD', toolCallId: 'c1', name: 'browser_tabs', args: { action: 'new' } }, signal()), stale);
 await assert.rejects(chat.onHostTool(conv, { sessionId: 'S1', toolCallId: 'c2', name: 'browser_tabs', args: { action: 'new' } }, signal()), stale);
 assert.equal(ran.length, 0);
});

test('an approval.request naming another turn shows no card', async () => {
 const { window, cards } = load();
 const { chat, conv, start } = chatWith(window);
 chat.setRemote(conv, start(), 'T1');
 await assert.rejects(chat.onApproval(conv, { sessionId: 'S1', turnId: 'T0', approvalId: 'a1', tool: 'bash', args: {} }, signal()), stale);
 assert.equal(cards.length, 0);
});

test('after Stop and a new turn, the old turn\'s requests cannot reach the new one', async () => {
 const { window, cards, ran } = load();
 const { chat, conv, start } = chatWith(window);
 const first = start();
 chat.setRemote(conv, first, 'A');
 const pending = chat.onApproval(conv, { sessionId: 'S1', turnId: 'A', approvalId: 'a1', tool: 'bash', args: {} }, signal());
 await tick();
 assert.equal(cards.length, 1);
 chat.abort(conv);
 assert.deepEqual(plain(await pending), { decision: 'deny', reason: 'cancelled' });
 // Stopped and not yet replaced: what still comes for A is cancelled.
 assert.deepEqual(plain(await chat.onApproval(conv, { sessionId: 'S1', turnId: 'A', approvalId: 'a2', tool: 'bash', args: {} }, signal())), { decision: 'deny', reason: 'cancelled' });
 await chat.end(conv, first, new DOMException('Aborted', 'AbortError'), null).catch(() => {});
 const second = start();
 chat.setRemote(conv, second, 'B');
 await assert.rejects(chat.onApproval(conv, { sessionId: 'S1', turnId: 'A', approvalId: 'a3', tool: 'bash', args: {} }, signal()), stale);
 await assert.rejects(chat.onHostTool(conv, { sessionId: 'S1', turnId: 'A', toolCallId: 't1', name: 'browser_tabs', args: { action: 'new' } }, signal()), stale);
 assert.equal(cards.length, 1);
 assert.equal(ran.length, 0);
 // B's own approval still works, and A's approval.resolved does not settle it.
 const own = chat.onApproval(conv, { sessionId: 'S1', turnId: 'B', approvalId: 'a1', tool: 'bash', args: {} }, signal());
 await tick();
 assert.equal(cards.length, 2);
 chat.onEvent(conv, 'approval.resolved', { sessionId: 'S1', turnId: 'A', approvalId: 'a1', decision: 'allow' });
 cards[1].settle('deny');
 assert.deepEqual(plain(await own), { decision: 'deny' });
});

test('a request before turn.start is answered waits for the turn id, then is checked against it', async () => {
 const { window, cards, ran } = load();
 const { chat, conv, start } = chatWith(window);
 const turn = start();
 const old = chat.onHostTool(conv, { sessionId: 'S1', turnId: 'OLD', toolCallId: 't0', name: 'browser_tabs', args: { action: 'new' } }, signal());
 const own = chat.onApproval(conv, { sessionId: 'S1', turnId: 'NEW', approvalId: 'a1', tool: 'bash', args: {} }, signal());
 await tick();
 assert.equal(cards.length, 0);
 assert.equal(ran.length, 0);
 chat.setRemote(conv, turn, 'NEW');
 await assert.rejects(old, stale);
 await tick();
 assert.equal(cards.length, 1);
 cards[0].settle('allow');
 assert.deepEqual(plain(await own), { decision: 'allow' });
 assert.equal(ran.length, 0);
});

test('a request waiting for the turn id is cancelled when the turn stops first', async () => {
 const { window, ran } = load();
 const { chat, conv, start } = chatWith(window);
 start();
 const waiting = chat.onHostTool(conv, { sessionId: 'S1', turnId: 'X', toolCallId: 't0', name: 'browser_tabs', args: { action: 'new' } }, signal());
 chat.abort(conv);
 assert.deepEqual(plain(await waiting), { status: 'cancelled', content: [] });
 assert.equal(ran.length, 0);
});

test('the same approval or tool call asked twice in a turn is refused the second time', async () => {
 const { window, cards, ran } = load();
 const { chat, conv, start } = chatWith(window);
 chat.setRemote(conv, start(), 'T1');
 chat.onApproval(conv, { sessionId: 'S1', turnId: 'T1', approvalId: 'a1', tool: 'bash', args: {} }, signal());
 await assert.rejects(chat.onApproval(conv, { sessionId: 'S1', turnId: 'T1', approvalId: 'a1', tool: 'bash', args: {} }, signal()), stale);
 assert.equal(cards.length, 1);
 chat.onHostTool(conv, { sessionId: 'S1', turnId: 'T1', toolCallId: 'c1', name: 'browser_tabs', args: { action: 'list' } }, signal());
 await tick();
 await assert.rejects(chat.onHostTool(conv, { sessionId: 'S1', turnId: 'T1', toolCallId: 'c1', name: 'browser_tabs', args: { action: 'list' } }, signal()), stale);
 assert.equal(ran.length, 1);
});

test('a browser step still running when its turn ends is cancelled and answers cancelled', async () => {
 const { window, ran, cancelled } = load();
 const { chat, conv, start } = chatWith(window);
 const turn = start();
 chat.setRemote(conv, turn, 'T1');
 const step = chat.onHostTool(conv, { sessionId: 'S1', turnId: 'T1', toolCallId: 'c1', name: 'browser_tabs', args: { action: 'list' } }, signal());
 await tick();
 assert.equal(ran.length, 1);
 await chat.end(conv, turn, null, null).catch(() => {});
 assert.deepEqual(plain(cancelled), [ran[0].id]);
 window.browserPanel.answer({ text: 'late' });
 assert.deepEqual(plain(await step), { status: 'cancelled', content: [] });
});
