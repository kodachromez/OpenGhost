'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { BackendHost, sessionMembers } = require('../desktop/backend-host');

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

test('non-JSON lines are dropped, split lines are joined, malformed envelopes reach the RPC peer', () => {
 const messages = [];
 const backend = new BackendHost({ command: null, onMessage: message => messages.push(message), log: quiet });
 backend.receive('{"jsonrpc":"2.0","method":"a"');
 backend.receive(',"params":{}}\nnot json\n{"x":1}\n');
 backend.receive('{"jsonrpc":"2.0","method":"b"}\n');
 assert.deepEqual(messages, [{ jsonrpc: '2.0', method: 'a', params: {} }, { x: 1 }, { jsonrpc: '2.0', method: 'b' }]);
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

// Audit F07: stop() always settles, once, for the child it was asked about, and never hangs a quit.
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');

// A child process the test drives by hand: it exits, closes and errors only when told to, and its kill can be made to fail.
function fakeChild({ pid = 4242, kill = () => true } = {}) {
 const child = new EventEmitter();
 Object.assign(child, { pid, exitCode: null, signalCode: null, stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(), kills: 0 });
 child.kill = signal => { child.kills++; return kill(signal); };
 return child;
}

function fakeHost(children, extra = {}) {
 const messages = [], statuses = [], logs = [];
 const backend = new BackendHost({
  command: { file: 'fake-backend', args: [] },
  onMessage: message => messages.push(message),
  onStatus: status => statuses.push(status),
  log: { error: line => logs.push(line) },
  spawnProcess: () => children.shift(),
  ...extra,
 });
 return { backend, messages, statuses, logs };
}

const within = (promise, ms) => Promise.race([promise.then(() => true), new Promise(resolve => setTimeout(() => resolve(false), ms))]);

test('stop() right after a spawn error settles instead of waiting for an exit that never comes', async () => {
 const { backend, statuses } = host({ command: { file: path.join(__dirname, 'no-such-backend'), args: [] } });
 backend.start();
 assert.ok(await within(backend.stop(5000), 1000), 'stop() did not settle after error/close');
 assert.equal(statuses.at(-1).state, 'error');
 assert.equal(backend.child, null);
 assert.equal(backend.stopping, null);
});

test('stop() settles on close even when no exit event arrives', async () => {
 const child = fakeChild();
 const { backend } = fakeHost([child]);
 backend.start();
 const stopping = backend.stop(5000);
 child.emit('close', 0, null);
 assert.ok(await within(stopping, 200));
});

test('overlapping stop() calls share one shutdown and all settle', async () => {
 const { backend, statuses, next } = host();
 backend.start();
 backend.send({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {} });
 await next(message => message.id === 1);
 const sent = [];
 const send = backend.send.bind(backend);
 backend.send = message => { sent.push(message.method); return send(message); };
 const first = backend.stop(), second = backend.stop(), third = backend.stop();
 assert.equal(first, second);
 assert.equal(first, third);
 await Promise.all([first, second, third]);
 assert.deepEqual(sent, ['shutdown']);
 assert.equal(statuses.filter(status => status.state === 'exited').length, 1);
 assert.equal(backend.stopping, null);
 // Normal shutdown is unchanged: a later stop has nothing to do, and the host can start a new backend.
 await backend.stop();
 backend.start();
 assert.ok(backend.child);
 await backend.stop();
 assert.equal(statuses.at(-1).state, 'exited');
});

test('a backend whose kill fails or never exits is given up after a bounded wait', async () => {
 const child = fakeChild({ pid: 2 ** 22 + 12345, kill: () => { throw new Error('kill failed'); } });
 const { backend, statuses } = fakeHost([child]);
 backend.start();
 child.emit('spawn');
 // The failed kill's error event must not crash or settle stop() early; only the bounded deadline does.
 const stopping = backend.stop(20);
 setTimeout(() => child.emit('error', new Error('kill EPERM')), 30);
 const started = Date.now();
 assert.ok(await within(stopping, 4000), 'stop() hung on a backend that would not die');
 assert.ok(Date.now() - started >= 20);
 assert.equal(child.kills, 1);
 assert.equal(backend.child, null);
 assert.equal(backend.stopping, null);
 assert.equal(statuses.at(-1).state, 'error');
});

test('stale events from an older backend do not affect a newer one', async () => {
 const old = fakeChild({ pid: 1001 }), fresh = fakeChild({ pid: 1002 });
 const { backend, messages, statuses, logs } = fakeHost([old, fresh]);
 backend.start();
 old.emit('spawn');
 // Half a line from the old backend must not prefix the new backend's first line.
 old.stdout.write('{"from":"old"');
 const stopping = backend.stop(5000);
 old.emit('exit', 0, null);
 await stopping;
 backend.start();
 assert.equal(backend.child, fresh);
 fresh.emit('spawn');
 assert.deepEqual(statuses.at(-1), { state: 'running', pid: 1002 });
 old.stdout.write('}\n{"from":"old-late"}\n');
 old.stderr.write('old noise\n');
 old.emit('spawn');
 old.emit('error', new Error('old error'));
 old.emit('exit', 1, null);
 old.emit('close', 1, null);
 fresh.stdout.write('{"from":"fresh"}\n');
 await new Promise(resolve => setImmediate(resolve));
 assert.deepEqual(messages, [{ from: 'fresh' }]);
 assert.deepEqual(logs, []);
 assert.deepEqual(statuses.at(-1), { state: 'running', pid: 1002 });
 assert.equal(backend.child, fresh);
 const stoppingFresh = backend.stop(5000);
 fresh.emit('exit', 0, null);
 await stoppingFresh;
 assert.equal(statuses.at(-1).state, 'exited');
});

test('start() during a shutdown does not spawn a second backend', async () => {
 const child = fakeChild();
 const spawned = [child];
 const { backend } = fakeHost(spawned);
 backend.start();
 child.emit('spawn');
 const stopping = backend.stop(5000);
 backend.start();
 assert.equal(backend.child, child);
 child.emit('exit', 0, null);
 await stopping;
});
