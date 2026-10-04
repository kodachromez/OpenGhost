'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { Writable } = require('node:stream');
const { BackendHost } = require('../desktop/backend-host');

const LIMIT = 64 * 1024 * 1024;
const message = { jsonrpc: '2.0', method: 'ok' };
const frame = Buffer.from(`${JSON.stringify(message)}\n`);

function host() {
 const messages = [], errors = [];
 const backend = new BackendHost({ onMessage: value => messages.push(value), log: { error: value => errors.push(value) } });
 return { backend, messages, errors };
}

// Valid JSON plus padding: the limit must apply before trimming as well as before parsing.
function paddedLine(bytes, value = message) {
 const line = Buffer.alloc(bytes + 1, 32);
 line.write(JSON.stringify(value));
 line[bytes] = 10;
 return line;
}

function outgoing(bytes, character = 'x') {
 const value = { jsonrpc: '2.0', method: 'send', params: { text: '' } };
 const room = bytes - Buffer.byteLength(JSON.stringify(value));
 const width = Buffer.byteLength(character);
 value.params.text = character.repeat(Math.floor(room / width)) + 'x'.repeat(room % width);
 return value;
}

// Withhold write callbacks to model a backend that does not read, then release them to model drain.
function blockedHost(t) {
 const { backend } = host();
 const writes = [], callbacks = [];
 const stdin = new Writable({
  highWaterMark: 1,
  write(chunk, encoding, callback) {
   writes.push(chunk);
   callbacks.push(callback);
  },
 });
 t.after(() => stdin.destroy());
 backend.child = { stdin };
 return { backend, stdin, writes, callbacks };
}

test('a complete oversized line is discarded before JSON.parse, even with its newline in the same chunk', t => {
 const { backend, messages, errors } = host();
 const parse = t.mock.method(JSON, 'parse');
 backend.receive(paddedLine(LIMIT + 1));
 assert.equal(parse.mock.callCount(), 0);
 assert.deepEqual(messages, []);
 assert.equal(errors.length, 1);
 backend.receive(frame);
 assert.deepEqual(messages, [message]);
});

test('a line exactly at the byte limit is accepted across chunks, including whitespace and CR', () => {
 const { backend, messages, errors } = host();
 const line = paddedLine(LIMIT);
 line[LIMIT - 1] = 13;
 backend.receive(line.subarray(0, LIMIT - 1));
 assert.deepEqual(messages, []);
 backend.receive(line.subarray(LIMIT - 1));
 assert.deepEqual(messages, [message]);
 assert.deepEqual(errors, []);
 assert.ok(backend.buffer.length <= LIMIT);
 assert.equal(backend.lineBytes, 0);
});

test('a split line that crosses the limit in its newline chunk is not parsed, and the next line survives', t => {
 const { backend, messages, errors } = host();
 const parse = t.mock.method(JSON, 'parse');
 const line = paddedLine(LIMIT + 1);
 backend.receive(line.subarray(0, LIMIT));
 assert.equal(errors.length, 0);
 backend.receive(Buffer.concat([line.subarray(LIMIT), frame]));
 assert.equal(errors.length, 1);
 assert.equal(parse.mock.callCount(), 1);
 assert.deepEqual(messages, [message]);
});

test('an oversized unterminated line releases its buffer and discards every suffix until newline', () => {
 const { backend, messages, errors } = host();
 backend.receive(Buffer.alloc(LIMIT, 32));
 backend.receive(Buffer.from(' '));
 assert.equal(backend.discarding, true);
 assert.equal(backend.buffer.length, 0);
 for (let i = 0; i < 10; i++) backend.receive(Buffer.alloc(1024, 32));
 backend.receive(Buffer.from('{"jsonrpc":"2.0","method":'));
 backend.receive(Buffer.from('"smuggled"}'));
 assert.equal(backend.lineBytes, 0);
 assert.equal(backend.buffer.length, 0);
 assert.equal(errors.length, 1);
 assert.deepEqual(messages, []);
 backend.receive(Buffer.concat([Buffer.from('\n'), frame, frame.subarray(0, 10)]));
 backend.receive(frame.subarray(10));
 assert.deepEqual(messages, [message, message]);
 assert.equal(backend.discarding, false);
});

test('UTF-8 characters split at every byte boundary are decoded only after framing', () => {
 const { backend, messages, errors } = host();
 const value = { jsonrpc: '2.0', method: 'é😀', params: { text: '日本語' } };
 const line = Buffer.from(`${JSON.stringify(value)}\n`);
 for (const byte of line) backend.receive(Buffer.from([byte]));
 assert.deepEqual(messages, [value]);
 assert.deepEqual(errors, []);
});

test('UTF-8 bytes, not UTF-16 code units, trigger discard before the newline arrives', t => {
 const { backend, messages, errors } = host();
 const parse = t.mock.method(JSON, 'parse');
 // The UTF-16 length is below LIMIT, but the wire length is LIMIT + 1.
 const line = paddedLine(LIMIT + 1, { jsonrpc: '2.0', method: 'é😀' });
 const split = line.indexOf(0xf0) + 2;
 backend.receive(line.subarray(0, split));
 backend.receive(line.subarray(split, -1));
 assert.equal(errors.length, 1);
 assert.equal(backend.discarding, true);
 assert.equal(parse.mock.callCount(), 0);
 backend.receive(Buffer.concat([line.subarray(-1), frame]));
 assert.deepEqual(messages, [message]);
});

test('outbound oversized ASCII and UTF-8 messages are rejected without writing or queueing', t => {
 const { backend, stdin, writes } = blockedHost(t);
 assert.equal(backend.send(outgoing(LIMIT + 1)), false);
 assert.equal(backend.send(outgoing(LIMIT + 1, 'é')), false);
 assert.equal(stdin.writableLength, 0);
 assert.deepEqual(writes, []);
 assert.equal(backend.send(message), true);
 assert.deepEqual(writes, [frame]);
});

test('an exact-limit outbound message is accepted once; a non-reading backend cannot grow the queue', async t => {
 const { backend, stdin, writes, callbacks } = blockedHost(t);
 assert.equal(backend.send(outgoing(LIMIT)), true);
 assert.equal(stdin.writableNeedDrain, true);
 assert.equal(stdin.writableLength, LIMIT + 1);
 assert.equal(writes[0].length, LIMIT + 1);
 assert.equal(writes[0].at(-1), 10);
 for (let i = 0; i < 1000; i++) assert.equal(backend.send(message), false);
 assert.equal(stdin.writableLength, LIMIT + 1);
 assert.equal(writes.length, 1);
 const drained = once(stdin, 'drain');
 callbacks.shift()();
 await drained;
 assert.equal(stdin.writableLength, 0);
 assert.equal(backend.send(message), true);
 assert.deepEqual(writes[1], frame);
 assert.equal(writes.length, 2);
 callbacks.shift()();
});

test('the outbound cap includes all queued frames and their newlines; drain preserves accepted FIFO order', async t => {
 const { backend, stdin, writes, callbacks } = blockedHost(t);
 assert.equal(backend.send(outgoing(LIMIT - 256)), true);
 assert.equal(stdin.writableNeedDrain, true);
 // Each of these fits by itself, but only the smaller one fits the remaining queue budget.
 assert.equal(backend.send(outgoing(256)), false);
 assert.equal(backend.send(outgoing(255)), true);
 assert.equal(stdin.writableLength, LIMIT + 1);
 assert.equal(writes.length, 1);
 const drained = once(stdin, 'drain');
 callbacks.shift()();
 assert.equal(writes.length, 2);
 assert.equal(writes[1].length, 256);
 callbacks.shift()();
 await drained;
 assert.equal(stdin.writableLength, 0);
 assert.equal(writes.length, 2);
});

// Audit F08: a backend that closes its stdin but keeps running. The write's own failure is reported, so the request it
// carried can fail at once, and later sends are refused synchronously.
test('send reports a failed stdin write through its callback, then refuses at once', async t => {
 const statuses = [];
 const backend = new BackendHost({
  command: { file: process.execPath, args: ['-e', 'require("fs").closeSync(0); setTimeout(() => {}, 10000)'] },
  onStatus: status => statuses.push(status.state), log: { error() {} },
 });
 backend.start();
 t.after(() => backend.child?.kill('SIGKILL'));
 await once(backend.child, 'spawn');
 // Give the child time to close its end of the pipe.
 let error;
 for (let attempt = 0; attempt < 50 && !error; attempt++) {
  await new Promise(resolve => setTimeout(resolve, 20));
  const written = new Promise(resolve => { if (!backend.send(message, resolve)) resolve('refused'); });
  error = await written;
 }
 assert.ok(error === 'refused' || error?.code === 'EPIPE', `unexpected write outcome ${error}`);
 assert.equal(backend.send(message, () => assert.fail('a refused send must not call back')), false);
 assert.deepEqual(statuses, ['running']);
});
