'use strict';

// F12: real retry/steering, RPC cancellation and turn cleanup; only rendering/storage are doubles.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, fakeTransport, tick } = require('./helpers');
const clone = value => JSON.parse(JSON.stringify(value));
const deferred = () => {
 let resolve, reject;
 const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
 return { promise, resolve, reject };
};
function node() {
 return {
  children: [], isConnected: true, classList: { remove() {}, add() {} }, style: {},
  append(...items) { for (const item of items) item.isConnected = true; this.children.push(...items); }, before() {}, remove() { this.isConnected = false; },
  replaceChildren() { this.children = []; }, getAnimations: () => [],
  addEventListener(type, fn) { this[type] = fn; },
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; },
  querySelectorAll(selector) {
   return this.children.flatMap(child => [child, ...child.querySelectorAll(selector)])
    .filter(child => selector.split(', ').some(part => child.className === part.slice(1)));
  },
 };
}
async function setup(t) {
 const transport = fakeTransport();
 const window = vm.createContext({
  console, setTimeout, clearTimeout, DOMException, AbortController, navigator: { language: 'en' },
  document: { readyState: 'complete', createElement: node }, openghost: { backend: transport },
  matchMedia: () => ({ matches: true }), I18n: { t: key => key, has: () => false }, Usage: { parts: () => null },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 await tick();
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1', capabilities: { sessions: { recovery: true } } } });
 const Backend = window.Backend;
 await Backend.ready;
 t.after(() => Backend.close({ state: 'exited' }));
 const config = { id: 'm', model: 'm', ready: true }, record = { id: 'S' };
 const conv = { id: 'S', record, messages: [], list: node(), reconciled: true, sessionVersion: 'V', tokens: 0 };
 const chat = Object.create(window.Chat.prototype), saves = [];
 Object.assign(chat, {
  active: conv, nodes: new Map(), config: () => config,
  library: { chat: () => record, saveMessages: (_id, messages) => saves.push(clone(messages)), update() {} },
  settings: { open() {} },
  checkpoint: async () => { saves.push(clone(conv.messages)); },
  assistantMessage: () => ({ el: { ...node(), isConnected: false }, stream: { push() {}, finish: async () => {} } }),
  userMessage: node, toolbar: node, restore() {}, dismissGhost() {}, showGhost() {},
  onChange() {}, followBottom() {}, sessionParams: async () => ({ model: 'm', userContext: { instructions: 'context' } }),
 });
 const calls = method => transport.sent.filter(call => call.method === method);
 const reply = (method, result, error) => {
  const call = calls(method).at(-1);
  assert.ok(call, `expected ${method}`);
  transport.deliver({ id: call.id, ...(error ? { error: { code: -32000, message: error.message, data: error } } : { result }) });
  return call;
 };
 let seq = 0;
 const event = (method, params = {}) => chat.onEvent(conv, method, { sessionId: 'S', turnId: conv.turn?.remote, seq: ++seq, ...params });
 const run = (prompt = { text: 'original', attachments: [] }) => {
  const done = chat.run(conv, prompt, config);
  const turn = conv.turn;
  return { turn, view: turn.part.view, done };
 };
 const start = async () => {
  const running = run();
  await tick();
  reply('turn.start', { turnId: 'T', sessionVersion: 'V' });
  await tick();
  return running;
 };
 const finish = async (error) => {
  event('turn.completed', { status: error ? 'error' : 'done', error });
  await tick();
 };
 const ready = () => { Backend.state = 'ready'; Backend.capabilities = { sessions: { recovery: true } }; };
 return { chat, conv, Backend, transport, calls, reply, event, run, start, finish, ready, saves };
}

for (const reason of ['unavailable', 'preparation', 'checkpoint']) test(`Retry of a known-undelivered ${reason} failure starts the original input, not history`, async t => {
 const f = await setup(t), attachment = deferred();
 const prompt = { text: 'keep my intent', attachments: [{ name: 'note.txt', ready: attachment.promise }] };
 if (reason === 'unavailable') f.Backend.close({ state: 'none' });
 const checkpoint = f.chat.checkpoint;
 if (reason === 'checkpoint') f.chat.checkpoint = async () => { throw new Error('disk full'); };
 const { turn, view, done } = f.run(prompt);
 if (reason === 'preparation') attachment.reject(new Error('read failed'));
 else attachment.resolve({ type: 'text', text: 'full attachment, not display preview' });
 await done;
 assert.equal(f.calls('turn.start').length, 0);
 assert.equal(f.conv.reconciled, true);
 assert.equal(f.conv.messages.some(entry => entry.pendingTurn), false);
 assert.ok(view.el.querySelector('.message-actions').children.some(item => item.textContent === 'chat.retry'));
 f.ready();
 f.chat.checkpoint = checkpoint;
 prompt.attachments[0].ready = Promise.resolve({ type: 'text', text: 'full attachment, not display preview' });
 f.chat.retry(f.conv, view);
 await tick();
 const call = f.calls('turn.start')[0];
 assert.equal(call.params.input.text, prompt.text);
 assert.equal(call.params.input.attachments[0].text, 'full attachment, not display preview');
 assert.equal(call.params.userContext.instructions, 'context');
 assert.notEqual(call.params.clientTurnId, turn.id);
 assert.equal(f.calls('turn.retry').length, 0);
 assert.equal(f.conv.messages.filter(entry => entry.role === 'user').length, 1);
 f.reply('turn.start', { turnId: 'T', sessionVersion: 'V' });
 await tick();
 await f.finish();
});

test('Retry of an accepted failed turn names that exact turn and keeps the target across an undelivered retry', async t => {
 const f = await setup(t), { view } = await f.start();
 f.event('message.started', { messageId: 'partial' });
 f.event('message.delta', { messageId: 'partial', text: 'already produced output' });
 await f.finish({ code: 'network', message: 'provider failed' });
 f.Backend.close({ state: 'exited' });
 f.chat.retry(f.conv, view);
 const retryView = f.conv.turn.part.view;
 await tick();
 assert.equal(f.calls('turn.retry').length, 0);
 f.ready();
 f.chat.retry(f.conv, retryView);
 await tick();
 const params = f.calls('turn.retry')[0].params;
 assert.equal(params.failedTurnId, 'T');
 assert.equal(params.sessionId, 'S');
 assert.equal(params.sessionVersion, 'V');
 assert.equal('input' in params, false);
 assert.ok(params.clientTurnId);
 assert.equal(f.calls('turn.start').length, 1);
 f.reply('turn.retry', { turnId: 'T2' });
 await tick();
 await f.finish();
});

test('lost start response recovers by its saved client identity; Retry never blindly resends', async t => {
 const f = await setup(t), { turn, view, done } = f.run();
 await tick();
 f.Backend.close({ state: 'exited' });
 await done;
 assert.equal(f.conv.reconciled, false);
 assert.equal(view.retryIntent, null);
 assert.ok(f.saves.some(messages => messages.some(entry => entry.backendTurn === turn.id && entry.pendingTurn)));
 f.ready();
 f.chat.retry(f.conv, view);
 await tick();
 assert.equal(f.calls('session.get')[0].params.clientTurnId, turn.id);
 f.reply('session.get', { exists: true, sessionVersion: 'V', revision: 1, turn: {
  clientTurnId: turn.id, turnId: 'ACCEPTED', input: { text: 'original', attachments: [] },
  events: [{ method: 'turn.completed', params: { sessionId: 'S', turnId: 'ACCEPTED', seq: 1, status: 'error', error: { message: 'failed after acceptance' } } }],
 } });
 await tick();
 assert.equal(f.calls('turn.start').length, 1);
 assert.equal(f.calls('turn.retry').length, 0);
 const errorBox = f.conv.list.querySelector('.message-error');
 assert.ok(errorBox);
 const action = f.conv.list.querySelector('.message-actions').children.find(item => item.textContent === 'chat.retry');
 action.click();
 await tick();
 assert.equal(f.calls('turn.retry')[0].params.failedTurnId, 'ACCEPTED');
 f.reply('turn.retry', { turnId: 'T2' });
 await tick();
 await f.finish();
});

test('invalid start acknowledgement and missing backend history fail closed rather than guessing a retry', async t => {
 const f = await setup(t), { view, done } = f.run();
 await tick();
 f.reply('turn.start', {});
 await done;
 assert.match(view.el.querySelector('.message-error').textContent, /did not identify/);
 assert.equal(view.retryIntent, null);
 f.chat.retry(f.conv, view);
 await tick();
 f.reply('session.get', { exists: false });
 await tick();
 assert.equal(f.conv.reconciled, false);
 assert.match(f.conv.recoveryNotice.textContent, /session is missing/);
 assert.equal(f.calls('turn.start').length, 1);
 assert.equal(f.calls('turn.retry').length, 0);
});

test('Retry requires context and matching session incarnation and honors explicit no-retry errors', async t => {
 const f = await setup(t);
 for (const error of [{ retryable: false }, { action: 'none' }, { code: 'invalid_params' }, { code: 'invalid_request' }]) {
  const view = { el: node(), retryIntent: { failedTurnId: 'T' }, retryVersion: 'V' };
  f.chat.fail(f.conv, view, error);
  assert.equal(view.el.querySelector('.message-actions').children.length, 0);
  f.chat.retry(f.conv, view);
 }
 f.chat.retry(f.conv, { el: node() });
 f.chat.retry(f.conv, { el: node(), retryIntent: { failedTurnId: 'T' }, retryVersion: 'old incarnation' });
 await tick();
 assert.equal(f.calls('turn.retry').length, 0);
 assert.equal(f.calls('turn.start').length, 0);
});

for (const error of [false, true]) test(`steering ${error ? 'RPC error' : 'accepted:false'} is visible and not silently retried`, async t => {
 const f = await setup(t), { turn } = await f.start();
 f.chat.interject(f.conv, { text: 'steer me', attachments: [] });
 const item = turn.queue[0];
 await tick();
 f.reply('turn.steer', { accepted: false }, error ? { code: 'invalid_input', message: 'bad steering input' } : null);
 await turn.steering;
 assert.equal(turn.queue.length, 0);
 assert.match(item.entry.inputError, error ? /bad steering input/ : /did not accept/);
 assert.equal(item.bubble.querySelector('.message-error').textContent, item.entry.inputError);
 f.event('input.accepted', { clientInputId: item.id }); // Rejected input cannot later be silently applied.
 assert.equal(turn.parts.length, 1);
 assert.ok(f.saves.at(-1).find(entry => entry.clientInputId === item.id).inputError);
 await f.finish();
 assert.equal(f.calls('turn.steer').length, 1);
});

test('steering dispatch and acceptance preserve input order despite reversed attachment readiness', async t => {
 const f = await setup(t), { turn } = await f.start(), first = deferred(), second = deferred();
 f.chat.interject(f.conv, { text: 'first', attachments: [{ name: 'one.txt', ready: first.promise }] });
 f.chat.interject(f.conv, { text: 'second', attachments: [{ name: 'two.txt', ready: second.promise }] });
 const items = [...turn.queue];
 second.resolve({ type: 'text', text: 'two' });
 await tick();
 assert.equal(f.calls('turn.steer').length, 0);
 first.resolve({ type: 'text', text: 'one' });
 await tick();
 assert.deepEqual(f.calls('turn.steer').map(call => call.params.input.text), ['first']);
 f.event('input.accepted', { clientInputId: items[0].id });
 f.reply('turn.steer', { accepted: true });
 await tick();
 assert.deepEqual(f.calls('turn.steer').map(call => call.params.input.text), ['first', 'second']);
 f.event('input.accepted', { clientInputId: items[1].id });
 f.reply('turn.steer', { accepted: true });
 await turn.steering;
 assert.deepEqual(Array.from(f.conv.messages.filter(entry => entry.clientInputId).map(entry => entry.text)), ['first', 'second']);
 assert.equal(turn.queue.length, 0);
 await f.finish();
});

for (const phase of ['attachment', 'start', 'rpc', 'stop', 'error']) test(`turn end aborts/drains steering waiting on ${phase}, with no late mutation or dispatch`, { timeout: 1000 }, async t => {
 const f = await setup(t), running = phase === 'start' ? f.run() : await f.start();
 const { turn, done } = running, attachment = deferred();
 if (phase === 'start') await tick();
 f.chat.interject(f.conv, { text: 'pending', attachments: phase === 'attachment' ? [{ name: 'late.txt', ready: attachment.promise }] : [] });
 f.chat.interject(f.conv, { text: 'also pending', attachments: [] });
 const items = [...turn.queue];
 await tick();
 if (phase === 'start') f.reply('turn.start', null, { message: 'start rejected' });
 else if (phase === 'stop') {
  f.chat.abort(f.conv);
  f.reply('turn.cancel', null);
 } else f.event('turn.completed', { status: phase === 'error' ? 'error' : 'done', error: { message: 'turn failed' } });
 await done;
 await turn.steering;
 assert.equal(turn.steeringController.signal.aborted, true);
 assert.equal(turn.queue.length, 0);
 assert.equal(f.Backend.pending.size, 0);
 for (const item of items) assert.match(item.entry.inputError, /not confirmed/);
 const saves = f.saves.length, calls = f.transport.sent.length;
 attachment.resolve({ type: 'text', text: 'too late' });
 const rpc = ['rpc', 'stop', 'error'].includes(phase);
 if (rpc) {
  const request = f.calls('turn.steer')[0];
  assert.ok(f.calls('$/cancelRequest').some(call => call.params.id === request.id));
  f.reply('turn.steer', { accepted: true });
 }
 await tick();
 assert.equal(f.saves.length, saves);
 assert.equal(f.transport.sent.length, calls);
 assert.equal(f.calls('turn.steer').length, rpc ? 1 : 0);
});

test('quiet-operation inputs keep their order when combined into a start, without concurrent steering', async t => {
 const f = await setup(t), gate = deferred(), first = deferred(), second = deferred();
 const turn = f.chat.begin(f.conv, f.chat.config(f.conv));
 turn.quiet = true;
 f.chat.openPart(f.conv, turn);
 const done = f.chat.drive(f.conv, turn, () => gate.promise);
 f.chat.interject(f.conv, { text: 'one', attachments: [{ name: '1', ready: first.promise }] });
 f.chat.interject(f.conv, { text: 'two', attachments: [{ name: '2', ready: second.promise }] });
 second.resolve({ type: 'text', text: 'second' });
 gate.resolve();
 await tick();
 assert.equal(f.calls('turn.start').length, 0);
 first.resolve({ type: 'text', text: 'first' });
 await tick();
 const input = f.calls('turn.start')[0].params.input;
 assert.equal(input.text, 'one\n\ntwo');
 assert.deepEqual(input.attachments.map(item => item.text), ['first', 'second']);
 assert.equal(f.calls('turn.steer').length, 0);
 f.reply('turn.start', { turnId: 'T', sessionVersion: 'V' });
 await tick();
 await f.finish();
 await done;
});
