#!/usr/bin/env node
'use strict';

// TEST FIXTURE ONLY: a scripted ABP v0 backend with no model behind it. It answers every turn with the same canned
// events so the tests can check the boundary (handshake, streaming, approvals, host tools, usage, cancellation) without
// any AI. It is never packaged (package.json build.files excludes test/**) and the app never starts it by itself.
//
// What a turn does depends on words in the message:
//   "approve" asks for an approval first; "browser" calls the browser_tabs host tool; "hang" never finishes (for Stop);
//   "fail" ends with an auth error; "crash" exits the process mid-turn.
const readline = require('node:readline');

const sessions = new Map();
const waiting = new Map();
let nextId = 1;
let seq = 0;

const send = message => process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', ...message })}\n`);
const event = (method, params) => send({ method, params: { seq: ++seq, ...params } });
const ask = (method, params) => new Promise(resolve => {
 const id = `b${nextId++}`;
 waiting.set(id, resolve);
 send({ id, method, params });
});

const CAPABILITIES = {
 turns: { steer: true, followUp: false, cancel: true, retry: true },
 thinking: { visible: false },
 tools: { events: true, hostTools: true },
 approvals: { modes: ['ask', 'auto', 'full'] },
 sessions: { list: false, delete: true, rename: false, side: true, edit: false, encrypted: false },
 compaction: { manual: true, auto: false, onModelSwitch: true },
 usage: { tokens: true, cost: false, context: true, limits: true },
 auth: { providers: true },
 attachments: { image: true, text: true, pdf: true, video: true, paths: true, maxBytes: 10485760 },
 titles: true,
 userContext: true,
};

let hello = null;

async function turn(sessionId, turnId, params) {
 const text = String(params.input?.text || '');
 const session = sessions.get(sessionId) || { turns: 0 };
 sessions.set(sessionId, session);
 session.turns++;
 session.current = turnId;
 const live = () => session.current === turnId;
 event('turn.started', { sessionId, turnId, clientTurnId: params.clientTurnId, model: params.model });
 const messageId = `${turnId}-m1`;
 event('message.started', { sessionId, turnId, messageId, role: 'assistant', model: params.model });
 for (const piece of ['Scripted ', 'reply ', `to: ${text}`]) event('message.delta', { sessionId, messageId, text: piece });
 if (text.includes('crash')) process.exit(3);
 if (text.includes('hang')) return;
 if (text.includes('approve')) {
  event('tool.started', { sessionId, turnId, toolCallId: 'k1', name: 'shell', title: 'List files' });
  const answer = await ask('approval.request', {
   sessionId, turnId, approvalId: 'p1', toolCallId: 'k1', tool: 'shell', args: { command: 'ls' },
   presentation: { kind: 'command', title: 'List the files here', effect: 'read', badge: true, code: 'ls', reveal: 'command' },
  });
  if (!live()) return;
  event('message.delta', { sessionId, messageId, text: `\n\nApproval: ${answer.result?.decision || 'error'}` });
 }
 if (text.includes('browser')) {
  const answer = await ask('host.tool', { sessionId, turnId, toolCallId: 'k2', name: 'browser_tabs', args: { action: 'list' } });
  if (!live()) return;
  event('message.delta', { sessionId, messageId, text: `\n\nBrowser: ${answer.result?.content?.[0]?.text || answer.error?.message || ''}` });
 }
 if (text.includes('fail')) {
  event('turn.completed', { sessionId, turnId, status: 'error', error: { code: 'auth', message: 'Test provider rejected the key', provider: 'Test provider', action: 'open-settings' } });
  return;
 }
 event('usage', { sessionId, turnId, messageId, provider: 'test', model: 'm1', modelName: 'Test model', input: 120, cached: 20, written: 0, output: 30, requests: 1, context: { used: 150, window: 1000 } });
 event('message.completed', { sessionId, messageId, finishReason: 'stop' });
 if (session.turns === 1) event('session.updated', { sessionId, title: 'Scripted title' });
 event('turn.completed', { sessionId, turnId, status: 'done', finishReason: 'stop' });
 session.current = null;
}

const handlers = {
 initialize(params) {
  hello = params;
  return { protocolVersion: '0.1', backend: { name: 'scripted-test-backend', version: '0', platform: process.platform }, capabilities: CAPABILITIES };
 },
 shutdown() {
  setImmediate(() => process.exit(0));
  return null;
 },
 // Echoes what the client said at the handshake, so a test can check it.
 'test.hello': () => hello,
 'auth.providers': () => [{
  id: 'test', name: 'Test provider', group: 'Test group', limits: true,
  methods: [{ type: 'apiKey', label: 'Test API key', hint: 'A key for the scripted backend.', url: 'https://example.invalid/keys', placeholder: 'test-…' }],
  status: { connected: true, keySaved: true },
 }],
 'auth.setKey': ({ key }) => ({ connected: !!key, keySaved: !!key }),
 'models.list': () => [{ id: 'm1', provider: 'test', name: 'Test model', contextWindow: 1000, vision: true, thinkingLevels: ['none', 'low', 'high'], defaultThinking: 'low' }],
 'account.limits': () => ({ plan: 'test', windows: [{ seconds: 18000, used: 25, resets: Date.now() + 3600000 }], balances: [{ currency: 'USD', total: 4.2 }] }),
 'turn.start'(params) {
  const turnId = `t${nextId++}`;
  setImmediate(() => turn(params.sessionId, turnId, params));
  return { turnId };
 },
 'turn.retry'(params) {
  const turnId = `t${nextId++}`;
  setImmediate(() => turn(params.sessionId, turnId, { ...params, input: { text: 'retry' } }));
  return { turnId };
 },
 'turn.steer'({ sessionId, turnId, clientInputId }) {
  setImmediate(() => event('input.accepted', { sessionId, turnId, clientInputId, messageId: `u-${clientInputId}` }));
  return { accepted: true };
 },
 'turn.cancel'({ sessionId, turnId }) {
  const session = sessions.get(sessionId);
  if (session?.current === turnId) session.current = null;
  setImmediate(() => event('turn.completed', { sessionId, turnId, status: 'cancelled' }));
  return null;
 },
 'session.configure': params => ({ model: params.model, thinking: params.thinking, permissionMode: params.permissionMode }),
 'session.compact'({ sessionId }) {
  event('compaction.started', { sessionId, reason: 'manual' });
  event('compaction.completed', { sessionId, ok: true });
  return { ok: true };
 },
 'session.delete': () => null,
};

readline.createInterface({ input: process.stdin }).on('line', async line => {
 if (!line.trim()) return;
 const message = JSON.parse(line);
 if (message.method === undefined) {
  const resolve = waiting.get(message.id);
  waiting.delete(message.id);
  resolve?.(message);
  return;
 }
 if (message.id === undefined) return;
 const handler = handlers[message.method];
 if (!handler) {
  send({ id: message.id, error: { code: -32601, message: `Method not found: ${message.method}` } });
  return;
 }
 try {
  send({ id: message.id, result: await handler(message.params || {}) });
 } catch (error) {
  send({ id: message.id, error: { code: -32000, message: error.message, data: { code: 'unknown', message: error.message } } });
 }
});
process.stderr.write('scripted-test-backend ready\n');
