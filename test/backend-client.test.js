'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { renderer, fakeTransport, tick } = require('./helpers');

// The client from backend-client.js, on a fake transport the test answers for the backend.
async function connected(capabilities = { turns: { cancel: true }, auth: { providers: true } }) {
 const transport = fakeTransport();
 const window = renderer(['backend-client.js'], { openghost: { backend: transport, platform: 'linux', desktop: true }, RenderGuide: 'GUIDE', HostTools: { schemas: [{ name: 'browser_tabs' }] } });
 await tick();
 const init = transport.last();
 transport.deliver({ id: init.id, result: { protocolVersion: '0.1', backend: { name: 'fake' }, capabilities } });
 assert.equal(await window.Backend.ready, true);
 return { window, transport, Backend: window.Backend };
}

test('without a backend bridge every request fails as backend_unavailable', async () => {
 const window = renderer(['backend-client.js'], {});
 assert.equal(await window.Backend.ready, false);
 assert.equal(window.Backend.available, false);
 await assert.rejects(window.Backend.request('models.list'), error => error.code === 'backend_unavailable' && /desktop app with a configured backend/.test(error.message));
});

test('a backend that is configured but not running stays unavailable', async () => {
 const transport = fakeTransport('none');
 const window = renderer(['backend-client.js'], { openghost: { backend: transport } });
 assert.equal(await window.Backend.ready, false);
 assert.equal(transport.sent.length, 0);
});

test('the handshake sends the protocol version, the host tools and the render guide', async () => {
 const { transport, Backend } = await connected();
 const init = transport.sent[0];
 assert.equal(init.method, 'initialize');
 assert.equal(init.params.protocolVersion, '0.1');
 assert.deepEqual(init.params.host.tools, [{ name: 'browser_tabs' }]);
 assert.equal(init.params.host.renderGuide, 'GUIDE');
 assert.equal(init.params.client.platform, 'linux');
 assert.equal(Backend.info.name, 'fake');
 assert.equal(Backend.can('turns.cancel'), true);
 assert.equal(Backend.can('compaction.manual'), false);
});

test('requests resolve with the result and reject with the ABP error', async () => {
 const { transport, Backend } = await connected();
 const ok = Backend.request('models.list', {});
 transport.deliver({ id: transport.last().id, result: [{ id: 'm' }] });
 assert.deepEqual(await ok, [{ id: 'm' }]);
 const bad = Backend.request('turn.start', {});
 transport.deliver({ id: transport.last().id, error: { code: -32000, message: 'x', data: { code: 'quota', message: 'No credit', provider: 'P' } } });
 await assert.rejects(bad, error => error.name === 'BackendError' && error.code === 'quota' && error.message === 'No credit' && error.provider === 'P');
 const missing = Backend.request('nope');
 transport.deliver({ id: transport.last().id, error: { code: -32601, message: 'Method not found' } });
 await assert.rejects(missing, error => error.code === 'unsupported');
});

test('an aborted request rejects at once and tells the backend with $/cancelRequest', async () => {
 const { transport, Backend } = await connected();
 const controller = new AbortController();
 const pending = Backend.request('session.compact', {}, { signal: controller.signal });
 const id = transport.last().id;
 controller.abort();
 await assert.rejects(pending, error => error.name === 'AbortError');
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', method: '$/cancelRequest', params: { id } });
});

test('events reach their listeners and the * listener', async () => {
 const { transport, Backend } = await connected();
 const seen = [], all = [];
 Backend.on('message.delta', params => seen.push(params.text));
 Backend.on('*', method => all.push(method));
 transport.deliver({ method: 'message.delta', params: { sessionId: 's', messageId: 'm', text: 'Hi' } });
 transport.deliver({ method: 'usage', params: { sessionId: 's' } });
 assert.deepEqual(seen, ['Hi']);
 assert.deepEqual(all, ['message.delta', 'usage']);
});

test('notifications are delivered only while ready and malformed envelopes do not reach listeners', async () => {
 const { transport, Backend } = await connected();
 const seen = [];
 Backend.on('*', method => seen.push(method));
 transport.deliver({ method: 'message.delta', params: 'bad' });
 transport.deliver({ method: 'message.delta', params: [] });
 transport.deliver({ method: 123, params: {} });
 transport.deliver({ method: 'models.changed' }); // No parameters is valid for this global notification.
 Backend.state = 'initializing';
 transport.deliver({ method: 'message.delta', params: { text: 'too early' } });
 transport.setStatus({ state: 'exited' });
 transport.deliver({ method: 'message.delta', params: { text: 'too late' } });
 assert.deepEqual(seen, ['models.changed']);
});

test('a failing notification listener cannot block other listeners or wildcard chat routing', async t => {
 const { transport, Backend } = await connected();
 const diagnostic = t.mock.method(console, 'error', () => {});
 const seen = [];
 Backend.on('message.delta', () => { throw new Error('listener failure'); });
 Backend.on('message.delta', p => seen.push(p.text));
 Backend.on('*', method => seen.push(method));
 assert.doesNotThrow(() => transport.deliver({ method: 'message.delta', params: { text: 'still routed' } }));
 assert.deepEqual(seen, ['still routed', 'message.delta']);
 assert.equal(diagnostic.mock.callCount(), 1);
});

test('reverse requests are answered by their handler, and $/cancelRequest aborts one', async () => {
 const { transport, Backend } = await connected();
 Backend.handle('approval.request', async params => ({ decision: params.tool === 'ok' ? 'allow' : 'deny' }));
 transport.deliver({ id: 'b1', method: 'approval.request', params: { tool: 'ok' } });
 await tick();
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', id: 'b1', result: { decision: 'allow' } });

 let aborted = false;
 Backend.handle('host.tool', (params, { signal }) => new Promise(resolve => signal.addEventListener('abort', () => { aborted = true; resolve(null); })));
 transport.deliver({ id: 'b2', method: 'host.tool', params: { name: 'browser_tabs' } });
 transport.deliver({ method: '$/cancelRequest', params: { id: 'b2' } });
 await tick();
 assert.equal(aborted, true);
 // As in JSON-RPC generally, a cancelled request is still answered, with what the handler made of the cancellation.
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', id: 'b2', result: null });

 transport.deliver({ id: 'b3', method: 'unknown.method', params: {} });
 await tick();
 assert.equal(transport.last().error.code, -32601);
});

test('a reverse request reusing the id of one still being answered is refused and its handler does not run', async () => {
 const { transport, Backend } = await connected();
 const calls = [];
 let finish;
 Backend.handle('host.tool', params => { calls.push(params.name); return new Promise(resolve => { finish = resolve; }); });
 transport.deliver({ id: 'b1', method: 'host.tool', params: { name: 'first' } });
 transport.deliver({ id: 'b1', method: 'host.tool', params: { name: 'second' } });
 await tick();
 assert.deepEqual(calls, ['first']);
 assert.equal(transport.last().error.data.code, 'duplicate_request');
 finish({ content: [] });
 await tick();
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', id: 'b1', result: { content: [] } });
});

test('a handler that throws answers with the ABP error', async () => {
 const { window, transport, Backend } = await connected();
 Backend.handle('host.tool', () => { throw new window.BackendError({ code: 'unsupported', message: 'No such tool' }); });
 transport.deliver({ id: 'b9', method: 'host.tool', params: {} });
 await tick();
 assert.deepEqual(transport.last().error.data, { code: 'unsupported', message: 'No such tool' });
});

test('a backend that exits fails what was waiting with backend_crashed', async () => {
 const { transport, Backend } = await connected();
 const closed = [];
 Backend.on('closed', status => closed.push(status));
 const pending = Backend.request('turn.start', {});
 transport.setStatus({ state: 'exited', code: 3 });
 await assert.rejects(pending, error => error.code === 'backend_crashed' && /exit code 3/.test(error.message));
 assert.equal(Backend.available, false);
 assert.equal(closed.length, 1);
 await assert.rejects(Backend.request('models.list'), error => error.code === 'backend_unavailable');
});

// Transport-only compatibility: production has no restart/reconnect command and does not emit this sequence.
test('a synthetic later running status still initializes the client again', async () => {
 const { transport, Backend } = await connected();
 transport.setStatus({ state: 'exited', code: 0 });
 transport.setStatus({ state: 'running' });
 await tick();
 const init = transport.last();
 assert.equal(init.method, 'initialize');
 transport.deliver({ id: init.id, result: { protocolVersion: '0.1', capabilities: { titles: true } } });
 await tick();
 assert.equal(Backend.available, true);
 assert.equal(Backend.can('titles'), true);
});

test('errors are worded for the chat, with a way to the settings when a key or sign-in is wrong', () => {
 const window = renderer(['backend-client.js'], {});
 const { explain } = window.Backend;
 assert.equal(explain({ code: 'auth', message: 'Bad key' }).settings, true);
 assert.equal(explain({ code: 'auth', message: 'Bad key' }).message, 'Bad key');
 assert.equal(explain({ code: 'server', message: 'x', action: 'open-settings' }).settings, true);
 assert.equal(explain({ code: 'network', message: 'Offline' }).settings, false);
 assert.equal(explain({ code: 'backend_unavailable', message: '' }).message, 'Something went wrong.');
});

// A client whose requests time out quickly: 20 ms for most methods, longer for session.compact.
async function hurried(timeouts = { default: 20, 'session.compact': 200 }) {
 const transport = fakeTransport();
 const window = renderer(['backend-client.js'], {});
 const Backend = new window.BackendClient(transport, { timeouts });
 await tick();
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1', backend: { name: 'fake' }, capabilities: {} } });
 assert.equal(await Backend.ready, true);
 return { transport, Backend };
}

const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

test('a request the backend never answers fails with timeout, and the backend is told to stop', async () => {
 const { transport, Backend } = await hurried();
 const pending = Backend.request('models.list', {});
 const id = transport.last().id;
 await assert.rejects(pending, error => error.name === 'BackendError' && error.code === 'timeout' && /models\.list/.test(error.message));
 assert.equal(Backend.pending.size, 0);
 assert.deepEqual(transport.last(), { jsonrpc: '2.0', method: '$/cancelRequest', params: { id } });
});

test('an answer after the timeout is ignored and does not touch later requests', async () => {
 const { transport, Backend } = await hurried();
 const late = Backend.request('models.list', {});
 const lateId = transport.last().id;
 await assert.rejects(late, error => error.code === 'timeout');
 const next = Backend.request('models.list', {});
 const nextId = transport.last().id;
 transport.deliver({ id: lateId, result: ['stale'] });
 transport.deliver({ id: lateId, error: { code: -32000, message: 'stale' } });
 assert.equal(Backend.pending.size, 1);
 transport.deliver({ id: nextId, result: ['fresh'] });
 assert.deepEqual(await next, ['fresh']);
 assert.equal(Backend.pending.size, 0);
 assert.equal(Backend.state, 'ready');
});

test('a request answered in time is unchanged: no timeout fires and no cancel is sent afterwards', async () => {
 const { transport, Backend } = await hurried();
 const ok = Backend.request('models.list', {});
 transport.deliver({ id: transport.last().id, result: [{ id: 'm' }] });
 assert.deepEqual(await ok, [{ id: 'm' }]);
 const sent = transport.sent.length;
 await wait(40);
 assert.equal(transport.sent.length, sent);
 assert.equal(Backend.pending.size, 0);
});

test('each method has its own deadline, and an abort before it still wins', async () => {
 const { transport, Backend } = await hurried();
 const slow = Backend.request('session.compact', {});
 const slowId = transport.last().id;
 await wait(40);
 assert.equal(Backend.pending.has(slowId), true);
 transport.deliver({ id: slowId, result: { ok: true } });
 assert.deepEqual(await slow, { ok: true });
 const controller = new AbortController();
 const aborted = Backend.request('models.list', {}, { signal: controller.signal });
 controller.abort();
 await assert.rejects(aborted, error => error.name === 'AbortError');
 const sent = transport.sent.length;
 await wait(40);
 assert.equal(transport.sent.length, sent);
});

test('a backend that never answers the handshake ends unavailable instead of initializing forever', async () => {
 const transport = fakeTransport();
 const window = renderer(['backend-client.js'], {});
 const Backend = new window.BackendClient(transport, { timeouts: { default: 20 } });
 assert.equal(await Backend.ready, false);
 assert.equal(Backend.state, 'unavailable');
 assert.equal(Backend.pending.size, 0);
 assert.match(Backend.unavailable().message, /initialization failed: The backend did not answer initialize in time.*relaunch OpenGhost/);
});

test('the shipped deadlines are bounded for every request, longest for sign-in and compaction', () => {
 const window = renderer(['backend-client.js'], {});
 const { timeouts } = new window.BackendClient(null);
 for (const ms of Object.values(timeouts)) assert.ok(Number.isFinite(ms) && ms > 0);
 assert.ok(timeouts['auth.login'] > timeouts.default && timeouts['session.compact'] > timeouts.default);
});
