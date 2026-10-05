'use strict';

// F09: real event admission, turn/message bookkeeping and end(), with rendering replaced by small view doubles.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, tick } = require('./helpers');

function node() {
 return {
  isConnected: true, classList: { remove() {} }, append() {}, before() {}, querySelector: () => null,
  remove() { this.isConnected = false; },
 };
}

function setup() {
 const ledger = [];
 const window = vm.createContext({
  console, setTimeout, clearTimeout, Promise, DOMException, AbortController,
  document: { readyState: 'complete' }, navigator: { language: 'en' }, matchMedia: () => ({ matches: true }),
  Usage: { record: p => ledger.push(p), parts: p => ({ input: p.input || 0, output: p.output || 0, requests: 1 }) },
  I18n: { t: key => key },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 window.Backend.request = async () => ({});
 const chat = Object.create(window.Chat.prototype);
 Object.assign(chat, {
  nodes: new Map(), library: { chat: () => null }, ghosts: 0,
  assistantMessage() {
   return { el: node(), status: node(), stream: { pushes: [], push(text) { this.pushes.push(text); }, finish: async () => {} } };
  },
  dismissGhost() {}, showGhost() { this.ghosts++; }, followBottom() {}, onChange() {}, note() {}, fail() {},
  toolbar: node, compactNotice: node, finishNotice() {},
 });
 const conv = chat.active = { id: 'S', messages: [], list: node(), turn: null, tokens: 0 };
 let seq = 0;
 const event = (method, params = {}) => chat.onEvent(conv, method, { sessionId: 'S', seq: ++seq, ...params });
 const start = (remote = 'T') => {
  const turn = chat.begin(conv, { id: 'model' });
  chat.openPart(conv, turn);
  if (remote) chat.setRemote(conv, turn, remote);
  return turn;
 };
 const message = (id, turnId = conv.turn.remote) => event('message.started', { turnId, messageId: id });
 const delta = (id, text, extra = {}) => event('message.delta', { messageId: id, text, ...extra });
 const usage = extra => event('usage', { provider: 'p', model: 'm', input: 10, ...extra });
 return { window, chat, conv, event, start, message, delta, usage, ledger };
}

test('duplicate sequences and backwards events are ignored; fresh identical deltas still stream', () => {
 const { conv, start, message, delta, event, usage, ledger } = setup();
 const turn = start();
 message('A');
 delta('A', 'DUP');
 delta('A', 'DUP', { seq: 2 });
 message('A'); // Even a second start under a fresh sequence must not reset its buffer.
 delta('A', 'DUP');
 delta('A', 'stale', { seq: 1 });
 delta('A', 'missing sequence', { seq: undefined });
 delta('A', 'invalid sequence', { seq: NaN });
 assert.equal(turn.part.entry.content, 'DUPDUP');
 event('message.completed', { messageId: 'A', text: 'final' });
 event('message.completed', { messageId: 'A', text: 'duplicate final' });
 delta('A', 'after final');
 usage({ turnId: 'T', messageId: 'A', seq: 100 });
 usage({ turnId: 'T', messageId: 'A', seq: 100 });
 assert.equal(turn.part.entry.content, 'final');
 assert.equal(turn.part.entry.usage.input, 10);
 assert.equal(ledger.length, 1);
 assert.equal(conv.eventSeq, 100);
});

test('interleaved messages finalize only their own buffers, in message-start order', () => {
 const { start, message, delta, event } = setup();
 const turn = start();
 message('A'); delta('A', 'alpha');
 message('B'); delta('B', 'beta');
 event('message.completed', { messageId: 'A', text: 'ALPHA' });
 assert.equal(turn.part.entry.content, 'ALPHA\n\nbeta');
 delta('B', ' tail');
 event('message.completed', { messageId: 'B', text: 'BETA' });
 assert.equal(turn.part.entry.content, 'ALPHA\n\nBETA');
 assert.deepEqual(Array.from(turn.part.view.stream.pushes), ['alpha', 'alpha\n\nbeta', 'ALPHA\n\nbeta', 'ALPHA\n\nbeta tail', 'ALPHA\n\nBETA']);
});

test('unknown-message deltas/finals are ignored; final without text seals a message and empty final clears only its own text', () => {
 const { start, message, delta, event } = setup();
 const turn = start();
 delta('A', 'before start');
 event('message.completed', { turnId: 'T', messageId: 'A', text: 'before start' });
 message('A'); delta('A', 'valid');
 event('message.completed', { messageId: 'A' });
 delta('A', 'after final');
 message('B'); delta('B', 'remove');
 event('message.completed', { messageId: 'B', text: '' });
 assert.equal(turn.part.entry.content, 'valid');
});

test('message-only deltas, finalization and usage wait with their start until the remote ID is known', () => {
 const { chat, conv, start, message, delta, event, usage, ledger } = setup();
 const turn = start(null);
 message('A', 'T'); delta('A', 'partial');
 event('message.completed', { messageId: 'A', text: 'final' });
 usage({ messageId: 'A' });
 assert.equal(turn.part.entry.content, '');
 assert.equal(ledger.length, 0);
 event('turn.started', { turnId: 'T', clientTurnId: turn.id });
 assert.equal(turn.part.entry.content, 'final');
 assert.equal(turn.part.entry.usage.input, 10);
 assert.equal(turn.early.length, 0);
 chat.setRemote(conv, turn, 'T'); // Matching RPC acknowledgement is idempotent.
 assert.equal(ledger.length, 1);
});

test('early buffering is bounded and foreign events cannot claim the local turn', () => {
 const { chat, conv, start, event } = setup();
 const turn = start(null);
 event('turn.started', { turnId: 'OLD' });
 event('turn.started', { turnId: 'OLD', clientTurnId: 'old-local' });
 assert.equal(turn.remote, '');
 for (let i = 0; i < 300; i++) event('message.delta', { turnId: 'OLD', messageId: 'old', text: 'x' });
 assert.equal(turn.early.length, 256);
 chat.setRemote(conv, turn, 'T');
 assert.equal(turn.part.entry.content, '');
 assert.equal(turn.early.length, 0);
 assert.throws(() => chat.setRemote(conv, turn, 'CONFLICT'), error => error.code === 'invalid_turn');
 assert.equal(turn.remote, 'T');
});

test('after Stop, old turn/message/tool/completion events cannot mutate output or a new turn', async () => {
 const { chat, conv, start, message, delta, event } = setup();
 const old = start('OLD');
 message('A'); delta('A', 'old text');
 chat.abort(conv);
 delta('A', 'same-task late');
 assert.equal(old.part.entry.content, 'old text');
 await chat.end(conv, old, new DOMException('Aborted', 'AbortError'));
 const current = start(null);
 event('turn.started', { turnId: 'OLD' });
 event('turn.started', { turnId: 'OLD', clientTurnId: old.id });
 assert.equal(current.remote, '');
 assert.throws(() => chat.setRemote(conv, current, 'OLD'), error => error.code === 'invalid_turn');
 chat.setRemote(conv, current, 'NEW');
 message('B'); delta('B', 'new text');
 for (const method of ['message.started', 'message.delta', 'message.completed', 'tool.started', 'turn.completed']) {
  event(method, { turnId: 'OLD', messageId: 'B', toolCallId: 'call', text: 'overwrite', status: 'done' });
 }
 delta('A', 'id-only stale');
 event('message.completed', { messageId: 'A', text: 'overwrite' });
 message('A', 'NEW'); // Reusing a retained message identity is refused too.
 assert.equal(current.messages.has('A'), false);
 assert.equal(old.part.entry.content, 'old text');
 assert.equal(current.part.entry.content, 'new text');
 assert.equal(current.terminal, undefined);
 assert.equal(chat.ghosts, 0);
});

for (const finalizeFirst of [true, false]) test(`turn completion ${finalizeFirst ? 'after' : 'before'} message finalization is terminal immediately`, async () => {
 const { chat, conv, start, message, delta, event } = setup();
 const turn = start();
 const driving = chat.drive(conv, turn, () => turn.done);
 message('A'); delta('A', 'streamed');
 if (finalizeFirst) event('message.completed', { messageId: 'A', text: 'final' });
 event('turn.completed', { turnId: 'T', status: 'done' });
 event('message.completed', { messageId: 'A', text: 'too late', finishReason: 'length' });
 delta('A', 'too late');
 message('B');
 event('turn.completed', { turnId: 'T', status: 'error', error: { message: 'duplicate completion' } });
 assert.equal(turn.part.entry.content, finalizeFirst ? 'final' : 'streamed');
 assert.equal(turn.messages.size, 1);
 assert.equal(await turn.done, null);
 await driving;
 assert.equal(conv.turn, null);
 event('message.completed', { messageId: 'A', text: 'after cleanup' });
 assert.equal(turn.part.entry.content, finalizeFirst ? 'final' : 'streamed');
});

test('tool starts are idempotent and cannot reopen a completed or foreign call', () => {
 const { chat, start, event } = setup();
 const turn = start();
 event('tool.started', { turnId: 'T', toolCallId: 'one' });
 event('tool.started', { turnId: 'T', toolCallId: 'one' });
 event('tool.completed', { turnId: 'T', toolCallId: 'one' });
 event('tool.started', { turnId: 'T', toolCallId: 'one' });
 event('tool.completed', { turnId: 'T', toolCallId: 'early' });
 event('tool.started', { turnId: 'T', toolCallId: 'early' });
 event('tool.started', { toolCallId: 'unowned' });
 event('tool.started', { turnId: 'OLD', toolCallId: 'foreign' });
 event('tool.started', { turnId: 'T', toolCallId: 'two' });
 assert.equal(chat.ghosts, 2);
 assert.equal(turn.tools.get('one'), 'completed');
 assert.equal(turn.tools.get('early'), 'completed');
});

test('message finalization and usage retain the original part across an accepted input', async () => {
 const { chat, conv, start, message, delta, event, usage, ledger } = setup();
 const turn = start();
 message('A'); delta('A', 'first');
 const first = turn.part;
 chat.takeQueue(conv, turn, [{ id: 'input', prompt: { text: 'steer', attachments: [] }, bubble: node() }]);
 message('B'); delta('B', 'second');
 event('message.completed', { messageId: 'A', text: 'FIRST' });
 usage({ turnId: 'T', messageId: 'A', input: 11 });
 usage({ messageId: 'B', input: 22 });
 usage({ turnId: 'T', input: 33 }); // Turn-only totals have a deterministic home: its first part.
 usage({ turnId: 'OTHER', messageId: 'B', input: 999 });
 usage({ turnId: 'T', messageId: 'missing', input: 999 });
 usage({ input: 999 });
 assert.equal(first.entry.content, 'FIRST');
 assert.equal(turn.part.entry.content, 'second');
 assert.equal(first.entry.usage.input, 44);
 assert.equal(turn.part.entry.usage.input, 22);
 assert.equal(ledger.length, 3);
 await tick();
});

for (const stopped of [false, true]) test(`late usage after ${stopped ? 'Stop' : 'completion'} is charged to the old message, not the new turn or its context`, async () => {
 const { chat, conv, start, message, delta, event, usage, ledger } = setup();
 const old = start('OLD');
 message('A'); delta('A', 'old');
 if (stopped) chat.abort(conv);
 else event('turn.completed', { turnId: 'OLD', status: 'done' });
 await chat.end(conv, old, stopped ? new DOMException('Aborted', 'AbortError') : null);
 usage({ turnId: 'OLD', messageId: 'A', input: 1 });
 const current = start('NEW');
 message('B'); delta('B', 'new');
 usage({ turnId: 'NEW', messageId: 'B', input: 2, context: { used: 20, window: 100 } });
 usage({ turnId: 'OLD', messageId: 'A', input: 3, context: { used: 999, window: 999 } });
 usage({ messageId: 'A', input: 4 });
 usage({ turnId: 'NEW', messageId: 'A', input: 999 });
 usage({ input: 999, context: { used: 999 } });
 assert.equal(old.part.entry.usage.input, 8);
 assert.equal(current.part.entry.usage.input, 2);
 assert.equal(conv.tokens, 20);
 assert.equal(conv.window, 100);
 assert.equal(ledger.length, 4);
});

test('an empty earlier part stays available for delayed text and cost-only final accounting', async () => {
 const { chat, conv, start, message, event, usage } = setup();
 const turn = start();
 message('A');
 const first = turn.part;
 chat.takeQueue(conv, turn, [{ id: 'input', prompt: { text: 'steer', attachments: [] }, bubble: node() }]);
 await tick();
 assert.equal(first.view.el.isConnected, true);
 event('message.completed', { messageId: 'A', text: 'delayed final' });
 message('B');
 event('turn.completed', { turnId: 'T', status: 'done' });
 await chat.end(conv, turn, null);
 usage({ messageId: 'B', input: 7 });
 assert.ok(conv.messages.includes(first.entry));
 assert.equal(first.entry.content, 'delayed final');
 assert.ok(conv.messages.includes(turn.part.entry));
 assert.equal(turn.part.entry.usage.input, 7);
});

test('replay deduplicates its own sequence without recharging usage, then resumes above the snapshot revision', () => {
 const { conv, start, message, delta, usage, ledger } = setup();
 const turn = start();
 conv.revision = conv.eventSeq = 100;
 turn.replaying = true;
 message('A'); delta('A', 'replay');
 usage({ turnId: 'T', messageId: 'A', seq: 3 });
 usage({ turnId: 'T', messageId: 'A', seq: 3 });
 assert.equal(turn.part.entry.usage.input, 10);
 assert.equal(ledger.length, 0);
 turn.replaying = false;
 usage({ turnId: 'T', messageId: 'A', seq: 3 });
 delta('A', ' live', { seq: 101 });
 usage({ turnId: 'T', messageId: 'A', seq: 102 });
 assert.equal(turn.part.entry.content, 'replay live');
 assert.equal(turn.part.entry.usage.input, 20);
 assert.equal(ledger.length, 1);
});

test('an accepted input with its own messageId is not mistaken for an assistant delta', () => {
 const { conv, start, event } = setup();
 const turn = start();
 turn.queue.push({ id: 'input', prompt: { text: 'steer', attachments: [] }, bubble: node() });
 event('input.accepted', { turnId: 'T', clientInputId: 'input', messageId: 'user-message' });
 event('input.accepted', { turnId: 'T', clientInputId: 'input', messageId: 'user-message' });
 assert.equal(turn.queue.length, 0);
 assert.equal(conv.messages.filter(entry => entry.role === 'user').length, 1);
});

test('a quiet operation cannot claim the queued ordinary turn it continues into', () => {
 const { start, event, usage, ledger } = setup();
 const turn = start(null);
 turn.quiet = true;
 event('compaction.started', { clientTurnId: turn.id });
 event('compaction.completed', { clientTurnId: turn.id, ok: true });
 // continueQueued reuses the local turn while transitioning out of quiet mode, before its remote ID arrives.
 turn.quiet = false;
 event('compaction.started', { clientTurnId: turn.id });
 usage({ clientTurnId: turn.id });
 assert.equal(turn.compaction, null);
 assert.equal(turn.part.entry.usage, undefined);
 assert.equal(ledger.length, 0);
});

test('manual compaction sends the identity that correlates its streamed notices and usage', async () => {
 const { window, chat, conv, event, usage } = setup();
 Object.defineProperty(chat, 'canCompact', { value: true });
 chat.config = () => ({ id: 'model' });
 let sent;
 window.Backend.request = async (method, params) => {
  sent = { method, params };
  event('compaction.started', { clientTurnId: params.clientTurnId });
  event('compaction.completed', { clientTurnId: params.clientTurnId, ok: true });
  usage({ clientTurnId: params.clientTurnId });
  return { ok: true };
 };
 assert.equal(chat.compactNow(), true);
 assert.equal(sent.method, 'session.compact');
 assert.equal(sent.params.clientTurnId, conv.turn.id);
 await tick();
 assert.equal(conv.turn, null);
 assert.equal(conv.messages.find(entry => entry.role === 'compact').usage.input, 10);
});

test('quiet compaction requires its client operation identity; session-only or late notices are ignored', async () => {
 const { chat, conv, start, event, usage, ledger } = setup();
 const quiet = start(null);
 quiet.quiet = true;
 event('compaction.started', {});
 assert.equal(quiet.compaction, null);
 event('compaction.started', { clientTurnId: quiet.id });
 assert.ok(quiet.compaction);
 event('compaction.completed', { clientTurnId: quiet.id, ok: true });
 usage({ clientTurnId: quiet.id, input: 5 });
 await chat.end(conv, quiet, null);
 const current = start('NEW');
 event('compaction.started', { clientTurnId: quiet.id });
 event('compaction.started', {});
 event('compaction.completed', { ok: true });
 usage({ clientTurnId: quiet.id, input: 6 });
 assert.equal(current.compaction, null);
 assert.equal(current.part.entry.usage, undefined);
 assert.equal(quiet.compactionEntry.usage.input, 11);
 assert.equal(ledger.length, 2);
});
