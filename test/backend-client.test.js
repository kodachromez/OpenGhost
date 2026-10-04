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
 await assert.rejects(window.Backend.request('models.list'), error => error.code === 'backend_unavailable');
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

test('a backend that comes back is greeted again', async () => {
 const { transport, Backend } = await connected();
 transport.setStatus({ state: 'exited', code: 0 });
 transport.setStatus({ state: 'running' });
 await tick();
 const init = transport.last();
 assert.equal(init.method, 'initialize');
 transport.deliver({ id: init.id, result: { capabilities: { titles: true } } });
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
