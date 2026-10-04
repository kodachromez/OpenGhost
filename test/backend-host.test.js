'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { BackendHost, configured, parseCommand, sessionMembers } = require('../desktop/backend-host');

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
 backend.send({ jsonrpc: '2.0', id: 2, method: 'turn.start', params: { sessionId: 's1', sessionVersion: null, clientTurnId: 'c1', input: { text: 'hello' } } });
 await next(message => message.method === 'turn.completed');
 await backend.stop();
 assert.equal(statuses.at(-1).state, 'exited');
});

test('the scripted recovery boundary retains an active turn across initialize and deduplicates creation', async () => {
 const { backend, messages, next } = host();
 backend.start();
 const request = async (id, method, params) => {
  backend.send({ jsonrpc: '2.0', id, method, params });
  return next(message => message.id === id);
 };
 try {
  await request('page1:1', 'initialize', { connectionId: 'page1' });
  const params = { sessionId: 's1', sessionVersion: null, clientTurnId: 'c1', input: { text: 'hang' } };
  const start = await request('page1:2', 'turn.start', params);
  await next(message => message.method === 'message.delta');
  await request('page2:1', 'initialize', { connectionId: 'page2' });
  const recovered = (await request('page2:2', 'session.get', { sessionId: 's1', clientTurnId: 'c1' })).result;
  assert.equal(recovered.sessionVersion, start.result.sessionVersion);
  assert.equal(recovered.turn.turnId, start.result.turnId);
  assert.equal(recovered.turn.input.text, 'hang');
  assert.ok(recovered.turn.events.some(event => event.method === 'message.delta'));
  const repeated = await request('page2:3', 'turn.start', params);
  assert.deepEqual(repeated.result, start.result);
  assert.equal(messages.filter(message => message.method === 'turn.started').length, 1);
  assert.deepEqual((await request('page2:4', 'session.get', { sessionId: 'missing' })).result, { exists: false });
 } finally {
  await backend.stop();
 }
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
 backend.send({ jsonrpc: '2.0', id: 1, method: 'turn.start', params: { sessionId: 's', sessionVersion: null, clientTurnId: 'c', input: { text: 'crash' } } });
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

// Audit F07: whatever the backend started goes with it, however the backend goes.
const TREE = path.join(__dirname, 'fixtures', 'tree-backend.js');
const alive = pid => { try { process.kill(pid, 0); return true; } catch { return false; } };
const pgid = pid => Number(fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' ')[2]);
const gone = async pid => {
 for (let i = 0; i < 100 && alive(pid) && !/\) Z /.test(fs.readFileSync(`/proc/${pid}/stat`, 'utf8')); i++) {
  await new Promise(resolve => setTimeout(resolve, 20));
 }
 return !alive(pid) || /\) Z /.test(fs.readFileSync(`/proc/${pid}/stat`, 'utf8'));
};

for (const mode of ['graceful', 'stubborn', 'crash']) {
 test(`a ${mode} backend leaves no descendants behind`, { skip: process.platform !== 'linux' && 'reads /proc' }, async () => {
  const { backend, statuses, next } = host({ command: { file: process.execPath, args: [TREE, mode] } });
  backend.start();
  const { params: { pids } } = await next(message => message.method === 'tree');
  try {
   const { pid } = backend.status;
   // One descendant shares the backend's process group and one has its own, like a backend's tools.
   assert.equal(pgid(pids[0]), pgid(pid));
   assert.notEqual(pgid(pids[1]), pgid(pid));
   if (mode === 'crash') {
    await new Promise(resolve => { const poll = () => statuses.at(-1).state === 'exited' ? resolve() : setTimeout(poll, 10); poll(); });
   } else {
    await backend.stop(mode === 'stubborn' ? 200 : 2000);
   }
   assert.equal(statuses.at(-1).state, 'exited');
   for (const descendant of pids) assert.ok(await gone(descendant), `pid ${descendant} is still alive`);
   assert.deepEqual(sessionMembers(pid), []);
  } finally {
   pids.forEach(descendant => { try { process.kill(descendant, 'SIGKILL'); } catch {} });
  }
 });
}
