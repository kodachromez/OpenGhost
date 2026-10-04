'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { BackendHost, configured, parseCommand } = require('../desktop/backend-host');

const FIXTURE = path.join(__dirname, 'fixtures', 'scripted-backend.js');
const quiet = { error() {} };

function host(extra = {}) {
 const messages = [], statuses = [];
 const backend = new BackendHost({
  command: { file: process.execPath, args: [FIXTURE] },
  cwd: __dirname,
  onMessage: message => messages.push(message),
  onStatus: status => statuses.push(status),
  log: quiet,
  ...extra,
 });
 const next = predicate => new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('timed out')), 5000);
  const poll = () => {
   const found = messages.find(predicate);
   if (found) { clearTimeout(timer); resolve(found); } else setTimeout(poll, 5);
  };
  poll();
 });
 return { backend, messages, statuses, next };
}

test('commands come from a path or a JSON array', () => {
 assert.deepEqual(parseCommand('/opt/ghosty'), { file: '/opt/ghosty', args: [] });
 assert.deepEqual(parseCommand('["/opt/ghosty", "--abp"]'), { file: '/opt/ghosty', args: ['--abp'] });
 assert.equal(parseCommand(''), null);
 assert.throws(() => parseCommand('[1]'));
});

test('the environment comes before backend.json, and neither means no backend', () => {
 const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'og-backend-'));
 try {
  assert.equal(configured({ env: {}, userData: dir }), null);
  fs.writeFileSync(path.join(dir, 'backend.json'), JSON.stringify({ command: ['/bin/b', '-x'] }));
  assert.deepEqual(configured({ env: {}, userData: dir }), { file: '/bin/b', args: ['-x'] });
  assert.deepEqual(configured({ env: { OPENGHOST_BACKEND: '/bin/a' }, userData: dir }), { file: '/bin/a', args: [] });
 } finally {
  fs.rmSync(dir, { recursive: true, force: true });
 }
});

test('with no command the host reports none and sends nothing', () => {
 const backend = new BackendHost({ log: quiet });
 assert.equal(backend.start().state, 'none');
 assert.equal(backend.send({ jsonrpc: '2.0', id: 1, method: 'initialize' }), false);
});

test('messages go to the backend as JSON lines and come back parsed', async () => {
 const { backend, statuses, next } = host();
 backend.start();
 await new Promise(resolve => setTimeout(resolve, 50));
 assert.equal(statuses.at(-1).state, 'running');
 assert.equal(backend.send({ not: 'json-rpc' }), false);
 assert.equal(backend.send({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '0.1' } }), true);
 const reply = await next(message => message.id === 1);
 assert.equal(reply.result.protocolVersion, '0.1');
 backend.send({ jsonrpc: '2.0', id: 2, method: 'turn.start', params: { sessionId: 's1', clientTurnId: 'c1', input: { text: 'hello' } } });
 await next(message => message.method === 'turn.completed');
 await backend.stop();
 assert.equal(statuses.at(-1).state, 'exited');
});

test('lines that are not JSON-RPC are dropped, split lines are joined', () => {
 const messages = [];
 const backend = new BackendHost({ command: null, onMessage: message => messages.push(message), log: quiet });
 backend.receive('{"jsonrpc":"2.0","method":"a"');
 backend.receive(',"params":{}}\nnot json\n{"x":1}\n');
 backend.receive('{"jsonrpc":"2.0","method":"b"}\n');
 assert.deepEqual(messages.map(message => message.method), ['a', 'b']);
});

test('a backend that dies mid-turn is reported as exited with its code', async () => {
 const { backend, statuses, next } = host();
 backend.start();
 backend.send({ jsonrpc: '2.0', id: 1, method: 'turn.start', params: { sessionId: 's', clientTurnId: 'c', input: { text: 'crash' } } });
 await next(message => message.method === 'message.delta');
 await new Promise(resolve => setTimeout(resolve, 200));
 assert.deepEqual({ state: statuses.at(-1).state, code: statuses.at(-1).code }, { state: 'exited', code: 3 });
});

test('a command that does not exist is an error, not a crash', async () => {
 const { backend, statuses } = host({ command: { file: path.join(__dirname, 'no-such-backend'), args: [] } });
 backend.start();
 await new Promise(resolve => setTimeout(resolve, 100));
 assert.equal(statuses.at(-1).state, 'error');
});
