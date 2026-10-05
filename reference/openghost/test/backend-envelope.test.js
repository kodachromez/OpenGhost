'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { BackendHost } = require('../desktop/backend-host');
const { validate } = require('../backend-protocol');
const { renderer, fakeTransport, tick } = require('./helpers');

async function peer(t, result = { protocolVersion: '0.1', backend: { name: 'fake' }, capabilities: {} }) {
 const transport = fakeTransport();
 const window = renderer(['backend-client.js'], { openghost: { backend: transport } });
 const Backend = window.Backend;
 t.after(() => Backend.close({ state: 'none' }));
 // Exercise the real JSONL ingress: malformed but parseable envelopes must reach the peer for rejection.
 const host = new BackendHost({ onMessage: message => Backend.receive(message), log: { error() {} } });
 const deliver = message => host.receive(`${JSON.stringify(message)}\n`);
 await tick();
 const closed = [];
 Backend.on('closed', status => closed.push(status));
 deliver({ jsonrpc: '2.0', id: transport.last().id, result });
 await Backend.ready;
 return { window, Backend, transport, deliver, closed };
}

const error = { code: -32000, message: 'Failure' };
const malformed = [
 null, [], [{ jsonrpc: '2.0', method: 'ok' }], true, 42, 'text', {},
 { jsonrpc: '1.0', method: 'ok' }, { method: 'ok' },
 { jsonrpc: '2.0' }, { jsonrpc: '2.0', method: 1 },
 ...[null, false, [], {}, 0.5, Number.MAX_SAFE_INTEGER + 1].map(id => ({ jsonrpc: '2.0', id, method: 'ok' })),
 ...[null, [], '', false, 1].map(params => ({ jsonrpc: '2.0', method: 'ok', params })),
 { jsonrpc: '2.0', method: 'ok', result: null },
 { jsonrpc: '2.0', method: 'ok', error },
 { jsonrpc: '2.0', result: null }, { jsonrpc: '2.0', error },
 { jsonrpc: '2.0', id: null, result: null },
 { jsonrpc: '2.0', id: 1 }, { jsonrpc: '2.0', id: 1, result: null, error },
 { jsonrpc: '2.0', id: 1, result: null, params: {} },
 ...[null, [], 'bad', {}, { code: -32000 }, { code: 'bad', message: 'x' }, { code: 0.5, message: 'x' }, { code: -32000, message: 1 }]
  .map(error => ({ jsonrpc: '2.0', id: 1, error })),
];

test('the shared validator and outbound host reject malformed envelopes, not valid traffic', () => {
 const writes = [];
 const host = new BackendHost();
 host.child = { stdin: { writable: true, writableLength: 0, write: line => writes.push(JSON.parse(line)) } };
 for (const message of malformed) {
  assert.ok(validate(message), JSON.stringify(message));
  assert.equal(host.send(message), false, JSON.stringify(message));
 }
 assert.equal(writes.length, 0);
 const valid = [
  { jsonrpc: '2.0', method: 'models.changed' },
  { jsonrpc: '2.0', method: 'new.event', params: {} },
  ...['', 'b1', 0, -1, Number.MAX_SAFE_INTEGER].map(id => ({ jsonrpc: '2.0', id, method: 'host.tool', params: {} })),
  ...[null, false, 0, '', [], {}].map(result => ({ jsonrpc: '2.0', id: 'r', result })),
  { jsonrpc: '2.0', id: 'r', error }, { jsonrpc: '2.0', id: null, error },
 ];
 for (const message of valid) {
  assert.equal(validate(message), null);
  assert.equal(host.send(message), true);
 }
 assert.deepEqual(writes, valid);
});

test('malformed notifications, primitives and batches never dispatch or receive replies', async t => {
 const { Backend, transport, deliver } = await peer(t);
 const seen = [];
 Backend.on('*', method => seen.push(method));
 const sent = transport.sent.length;
 for (const message of malformed.filter(message => !message || !Object.hasOwn(message, 'id'))) deliver(message);
 assert.deepEqual(seen, []);
 assert.equal(transport.sent.length, sent);
 deliver({ jsonrpc: '2.0', method: 'models.changed' });
 deliver({ jsonrpc: '2.0', method: 'future.event', params: { ok: true } });
 assert.deepEqual(seen, ['models.changed', 'future.event']);
});

test('invalid response shapes reject only their matching call with protocol_error, without replying', async t => {
 const { Backend, transport, deliver } = await peer(t);
 const other = Backend.request('other');
 const otherId = transport.last().id;
 const shapes = [
  {}, { result: null, error }, { result: null, params: {} },
  { jsonrpc: '1.0', result: null }, { jsonrpc: null, result: null },
  { method: 'host.tool', result: null }, { method: null, error },
  ...[null, [], '', {}, { code: -32000 }, { message: 'x' }, { code: 'x', message: 'x' },
   { code: 0.5, message: 'x' }, { code: Number.MAX_SAFE_INTEGER + 1, message: 'x' }, { code: -32000, message: {} }].map(error => ({ error })),
 ];
 let executed = false;
 Backend.handle('host.tool', () => { executed = true; });
 for (const shape of shapes) {
  const pending = Backend.request('models.list');
  const id = transport.last().id, sent = transport.sent.length;
  const rejected = assert.rejects(pending, e => e.name === 'BackendError' && e.code === 'protocol_error');
  deliver({ jsonrpc: '2.0', id, ...shape });
  await rejected;
  assert.equal(Backend.pending.has(id), false);
  assert.equal(Backend.pending.has(otherId), true);
  assert.equal(transport.sent.length, sent);
 }
 // These cannot occur in JSON but can arrive over a structured-clone bridge.
 for (const shape of [{ result: undefined }, { error: undefined }, { jsonrpc: undefined, result: null }]) {
  const pending = Backend.request('models.list');
  Backend.receive({ jsonrpc: '2.0', id: transport.last().id, ...shape });
  await assert.rejects(pending, e => e.code === 'protocol_error');
 }
 assert.equal(executed, false);
 deliver({ jsonrpc: '2.0', id: otherId, result: false });
 assert.equal(await other, false);
 const ok = Backend.request('turn.cancel');
 deliver({ jsonrpc: '2.0', id: transport.last().id, result: null });
 assert.equal(await ok, null);
 assert.equal(Backend.available, true);
});

test('missing, invalid, mismatched, unknown and late response IDs never settle another call', async t => {
 const { Backend, transport, deliver } = await peer(t);
 const pending = Backend.request('models.list');
 const id = transport.last().id, sent = transport.sent.length;
 for (const fields of [{}, ...[null, false, {}, [], 0.5, Number.MAX_SAFE_INTEGER + 1, 'unknown', 1, `${id}:wrong`].map(id => ({ id }))]) {
  deliver({ jsonrpc: '2.0', ...fields, result: ['wrong'] });
  deliver({ jsonrpc: '2.0', ...fields, error });
 }
 assert.equal(Backend.pending.size, 1);
 assert.equal(transport.sent.length, sent);
 deliver({ jsonrpc: '2.0', id, result: ['right'] });
 assert.deepEqual(await pending, ['right']);
 deliver({ jsonrpc: '2.0', id, error });
 assert.equal(Backend.pending.size, 0);
 assert.equal(transport.sent.length, sent);
 // Response matching is type-sensitive, even for a peer using numeric IDs.
 let resolved = false;
 Backend.pending.set(7, { resolve: () => { resolved = true; } });
 deliver({ jsonrpc: '2.0', id: '7', result: null });
 assert.equal(resolved, false);
 deliver({ jsonrpc: '2.0', id: 7, result: null });
 assert.equal(resolved, true);
});

test('malformed reverse requests fail before handlers run with -32600 or -32602', async t => {
 const { Backend, transport, deliver } = await peer(t);
 let calls = 0;
 Backend.handle('host.tool', () => { calls++; return {}; });
 const cases = [
  ...[null, [], '', false, 1].map(params => [{ params }, -32602]),
  ...[null, false, {}, [], 0.5, Number.MAX_SAFE_INTEGER + 1].map(id => [{ id }, -32600]),
  ...[null, 1, {}, []].map(method => [{ method }, -32600]),
  [{ jsonrpc: '1.0' }, -32600], [{ result: null }, -32600], [{ error }, -32600],
 ];
 for (const [fields, code] of cases) {
  const message = { jsonrpc: '2.0', id: 'reverse', method: 'host.tool', params: {}, ...fields };
  const sent = transport.sent.length;
  deliver(message);
  assert.equal(transport.sent.length, sent + 1);
  assert.equal(transport.last().id, typeof message.id === 'string' || Number.isSafeInteger(message.id) ? message.id : null);
  assert.equal(transport.last().error.code, code);
  assert.equal(transport.last().error.data.code, 'invalid_request');
 }
 assert.equal(calls, 0);
 deliver({ jsonrpc: '2.0', id: 0, method: 'host.tool', params: {} });
 await tick();
 assert.equal(calls, 1);
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', id: 0, result: {} });
});

test('malformed cancellation notifications cannot abort reverse calls; numeric and string IDs stay distinct', async t => {
 const { Backend, transport, deliver } = await peer(t);
 const calls = [];
 Backend.handle('host.tool', (params, { signal }) => new Promise(resolve => calls.push({ signal, resolve })));
 for (const id of [7, '7']) deliver({ jsonrpc: '2.0', id, method: 'host.tool' });
 for (const fields of [{}, { params: null }, { params: [] }, { params: { id: null } }, { params: { id: {} } }, { params: { id: 'unknown' } }, { params: { id: 7 }, result: null }]) {
  deliver({ jsonrpc: '2.0', method: '$/cancelRequest', ...fields });
 }
 assert.deepEqual(calls.map(call => call.signal.aborted), [false, false]);
 deliver({ jsonrpc: '2.0', method: '$/cancelRequest', params: { id: '7' } });
 assert.deepEqual(calls.map(call => call.signal.aborted), [false, true]);
 deliver({ jsonrpc: '2.0', method: '$/cancelRequest', params: { id: 7 } });
 assert.deepEqual(calls.map(call => call.signal.aborted), [true, true]);
 calls.forEach(call => call.resolve(null));
 await tick();
 assert.deepEqual(transport.sent.slice(-2).map(message => message.id), [7, '7']);
});

test('missing/malformed or unsupported protocolVersion never makes the client ready or enables reverse dispatch', async t => {
 for (const result of [null, {}, [], '0.1', { protocolVersion: 0.1 }, { protocolVersion: '999' }, { protocolVersion: '' }]) {
  await t.test(JSON.stringify(result), async t => {
   const { Backend, transport, deliver, closed } = await peer(t, result);
   assert.equal(await Backend.ready, false);
   assert.equal(Backend.available, false);
   assert.equal(Backend.info, null);
   assert.deepEqual(Object.keys(Backend.capabilities), []);
   assert.equal(closed.length, 1);
   assert.match(closed[0].error, /protocolVersion/);
   await assert.rejects(Backend.request('models.list'), e => e.code === 'backend_unavailable');
   let called = false;
   Backend.handle('host.tool', () => { called = true; });
   deliver({ jsonrpc: '2.0', id: 'reverse', method: 'host.tool' });
   assert.equal(called, false);
   assert.equal(transport.last().error.data.code, 'backend_unavailable');
  });
 }
});

test('hello cannot override the supported protocolVersion', async t => {
 const transport = fakeTransport();
 const window = renderer(['backend-client.js']);
 const Backend = new window.BackendClient(transport, { hello: () => ({ protocolVersion: '999' }) });
 t.after(() => Backend.close({ state: 'none' }));
 await tick();
 assert.equal(transport.last().params.protocolVersion, '0.1');
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1' } });
 assert.equal(await Backend.ready, true);
});

test('standard RPC errors and named ABP errors map consistently in both directions', async t => {
 const { window, Backend, transport, deliver } = await peer(t);
 for (const [code, expected] of [[-32700, 'invalid_request'], [-32600, 'invalid_request'], [-32602, 'invalid_request'], [-32601, 'unsupported'], [-32603, 'unknown'], [-32099, 'unknown']]) {
  const pending = Backend.request('unknown');
  deliver({ jsonrpc: '2.0', id: transport.last().id, error: { code, message: 'RPC failure', data: { code: 123, message: {} } } });
  await assert.rejects(pending, e => e.code === expected && e.message === 'RPC failure');
 }
 const pending = Backend.request('known');
 deliver({ jsonrpc: '2.0', id: transport.last().id, error: { code: -32000, message: 'RPC', data: { code: 'custom', message: 'ABP', retryable: false } } });
 await assert.rejects(pending, e => e.code === 'custom' && e.message === 'ABP' && e.retryable === false);
 deliver({ jsonrpc: '2.0', id: 'missing', method: 'unknown.method' });
 assert.equal(transport.last().error.code, -32601);
 assert.equal(transport.last().error.data.code, 'unsupported');
 for (const [thrown, code, appCode] of [
  [new window.BackendError({ code: 'unsupported', message: 'No tool' }), -32601, 'unsupported'],
  [new window.BackendError({ code: 'invalid_request', message: 'Bad args' }), -32602, 'invalid_request'],
  [new window.BackendError({ code: 'custom', message: 'Custom' }), -32000, 'custom'],
  [new Error('Failure'), -32000, 'unknown'],
  [new DOMException('Aborted', 'AbortError'), -32000, 'cancelled'],
 ]) {
  Backend.handle('host.tool', () => { throw thrown; });
  deliver({ jsonrpc: '2.0', id: 'reverse', method: 'host.tool' });
  await tick();
  assert.equal(transport.last().error.code, code);
  assert.equal(transport.last().error.data.code, appCode);
 }
});
