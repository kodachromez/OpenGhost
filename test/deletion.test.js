'use strict';

// Audit F03: exercise the real list -> chat -> library deletion path with a controllable backend.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { ROOT, fakeTransport, tick } = require('./helpers');

function node() {
 return {
  style: { overflow: '' }, offsetHeight: 40, inert: false, removed: false,
  remove() { this.removed = true; }, contains(other) { return this === other; },
  animate(frames, options) {
   this.animation = { frames, options, finished: Promise.resolve(), cancelled: false, cancel() { this.cancelled = true; } };
   return this.animation;
  },
 };
}

async function setup({ reduced = true, loaded = true } = {}) {
 const errors = [], released = [], removed = [], aborted = [];
 const window = vm.createContext({
  console, setTimeout, clearTimeout, Promise, DOMException, AbortController,
  navigator: { language: 'en-US' }, document: { readyState: 'complete' },
  matchMedia: () => ({ matches: reduced }), alert: message => errors.push(message),
  openghost: { platform: 'linux', releaseFolder: async path => released.push(path) },
 });
 window.window = window;
 for (const file of ['backend-protocol.js', 'backend-client.js', 'library.js', 'chat.js', 'chat-list.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 }
 const transport = fakeTransport(), explain = window.Backend.explain;
 window.Backend = new window.BackendClient(transport);
 window.Backend.explain = explain;
 await tick();
 transport.deliver({ id: transport.last().id, result: { protocolVersion: '0.1', capabilities: { sessions: { delete: true, side: true } } } });
 await window.Backend.ready;
 const records = [
  { id: 'a', folder: '/project' }, { id: 'b', folder: '/project' },
  { id: 'other', folder: '/elsewhere' },
 ];
 const data = new Map(records.flatMap(record => [[`chats/${record.id}`, 'messages'], [`mini/${record.id}`, 'side messages']]));
 const library = Object.assign(Object.create(window.Library.prototype), {
  chats: records, folders: [{ path: '/project', name: 'Project' }, { path: '/elsewhere' }], home: { path: '/Chats' },
  keys: new Map(), titles: new Map(),
  store: { remove: async key => { removed.push(key); data.delete(key); } },
  changed() {},
 });
 const conversations = new Map((loaded ? [records[0], records[2]] : [records[2]]).map(record => [record.id, { id: record.id, record, list: node() }]));
 const selected = conversations.get('a'), other = conversations.get('other');
 const chat = Object.assign(Object.create(window.Chat.prototype), {
  library, conversations, active: other, draft: null,
  abort: conv => aborted.push(conv.id),
  newChat(folder) { this.active = this.draft = { id: '', record: null, folder }; },
 });
 const list = Object.assign(Object.create(window.ChatList.prototype), {
  library, chat, rows: new Map(records.map(record => [record.id, { row: node() }])),
  hovered: null, wake() {},
 });
 const group = { path: '/project', section: node() };
 const requests = () => transport.sent.filter(message => message.method === 'session.delete');
 const ids = () => requests().map(message => message.params.sessionId);
 const answer = async (id, error) => {
  const request = requests().at(-1);
  assert.equal(request?.params.sessionId, id);
  transport.deliver({ id: request.id, ...(error ? { error: { code: -32000, message: error } } : { result: null }) });
  await tick();
 };
 return { window, transport, library, chat, list, group, selected, other, errors, released, removed, aborted, data, requests, ids, answer };
}

for (const reduced of [true, false]) {
 for (const loaded of [true, false]) {
  test(`deleting an inactive ${loaded ? 'loaded' : 'unloaded'} chat waits for its exact sessions (reduced motion: ${reduced})`, async () => {
   const f = await setup({ reduced, loaded });
   const pending = f.list.remove('a');
   await tick();
   assert.deepEqual(f.ids(), ['a']);
   assert.ok(f.library.chat('a'));
   assert.deepEqual(f.removed, []);
   await f.answer('a');
   assert.deepEqual(f.ids(), ['a', 'a:mini']);
   assert.ok(f.library.chat('a'), 'keep the record until both sessions are acknowledged');
   await f.answer('a:mini');
   await pending;
   assert.equal(f.library.chat('a'), null);
   assert.deepEqual(f.removed, ['chats/a', 'mini/a']);
   assert.equal(f.chat.conversations.has('a'), false);
   if (loaded) assert.equal(f.selected.list.removed, true);
   assert.equal(f.chat.active, f.other);
   assert.ok(f.library.chat('b'));
   assert.ok(f.data.has('chats/other'));
   assert.deepEqual(f.errors, []);
  });
 }

 test(`folder deletion keeps IDs until ACK and removes only its children (reduced motion: ${reduced})`, async () => {
  const f = await setup({ reduced });
  f.chat.active = f.selected;
  const pending = f.list.removeFolder(f.group);
  await tick();
  assert.ok(f.library.chat('a'));
  assert.ok(f.library.chat('b'));
  for (const id of ['a', 'a:mini', 'b', 'b:mini']) await f.answer(id);
  await pending;
  assert.deepEqual(f.ids(), ['a', 'a:mini', 'b', 'b:mini']);
  assert.deepEqual(f.library.chats.map(record => record.id), ['other']);
  assert.deepEqual(f.library.folders.map(folder => folder.path), ['/elsewhere']);
  assert.deepEqual(f.removed, ['chats/a', 'mini/a', 'chats/b', 'mini/b']);
  assert.equal(f.chat.active.folder, null, 'the active deleted chat becomes a draft outside the deleted folder');
  assert.equal(f.other.list.removed, false);
  assert.deepEqual(f.errors, []);
 });

 test(`a rejected chat deletion is visible, restores the row, and can retry (reduced motion: ${reduced})`, async () => {
  const f = await setup({ reduced });
  f.chat.active = f.selected;
  const row = f.list.rows.get('a').row;
  const pending = f.list.remove('a');
  await tick();
  await f.answer('a', 'Deletion denied');
  await pending;
  assert.deepEqual(f.errors, ['Deletion denied']);
  assert.ok(f.library.chat('a'));
  assert.equal(f.chat.active, f.selected);
  assert.equal(f.selected.list.removed, false);
  assert.deepEqual(f.removed, []);
  assert.equal(row.inert, false);
  assert.equal(row.__removing, false);
  assert.equal(row.style.overflow, '');
  if (!reduced) assert.equal(row.animation.cancelled, true);
  const retry = f.list.remove('a');
  await tick();
  await f.answer('a');
  await f.answer('a:mini');
  await retry;
  assert.equal(f.library.chat('a'), null);
  assert.equal(f.chat.active.folder.path, '/project');
 });
}

test('a partly failed folder delete commits successful children but retains the failed and unattempted children for retry', async () => {
 const f = await setup({ reduced: false });
 f.library.chats.push({ id: 'c', folder: '/project' });
 const pending = f.list.removeFolder(f.group);
 await tick();
 await f.answer('a');
 await f.answer('a:mini');
 await f.answer('b', 'Backend storage failed');
 await pending;
 assert.deepEqual(f.errors, ['Backend storage failed']);
 assert.equal(f.library.chat('a'), null);
 assert.ok(f.library.chat('b'));
 assert.ok(f.library.chat('c'));
 assert.ok(f.library.folders.some(folder => folder.path === '/project'));
 assert.deepEqual(f.ids(), ['a', 'a:mini', 'b']);
 assert.equal(f.group.section.inert, false);
 assert.equal(f.group.section.animation.cancelled, true);
 const retry = f.list.removeFolder(f.group);
 await tick();
 for (const id of ['b', 'b:mini', 'c', 'c:mini']) await f.answer(id);
 await retry;
 assert.deepEqual(f.library.chats.map(record => record.id), ['other']);
});

test('mini-session deletion failure keeps the parent record and local histories', async () => {
 const f = await setup();
 const pending = f.list.remove('a');
 await f.answer('a');
 await f.answer('a:mini', 'Mini deletion failed');
 await pending;
 assert.deepEqual(f.errors, ['Mini deletion failed']);
 assert.ok(f.library.chat('a'));
 assert.deepEqual(f.removed, []);
 const retry = f.list.remove('a');
 await f.answer('a'); // An already-deleted session must acknowledge idempotently.
 await f.answer('a:mini');
 await retry;
 assert.equal(f.library.chat('a'), null);
});

for (const state of ['offline', 'unsupported', 'timeout']) {
 test(`${state} deletion surfaces a failure without removing local state`, async () => {
  const f = await setup();
  if (state === 'offline') f.window.Backend.close({ state: 'none' });
  if (state === 'unsupported') f.window.Backend.capabilities.sessions.delete = false;
  if (state === 'timeout') f.window.Backend.timeouts['session.delete'] = 5;
  await f.list.remove('a');
  assert.equal(f.errors.length, 1);
  assert.match(f.errors[0], state === 'offline' ? /No backend/ : state === 'unsupported' ? /does not support/ : /did not answer/);
  assert.ok(f.library.chat('a'));
  assert.deepEqual(f.removed, []);
  assert.equal(f.list.rows.get('a').row.inert, false);
 });
}

test('repeated and overlapping chat/folder confirmations cannot send duplicate deletes', async () => {
 const f = await setup();
 const pending = f.list.remove('a');
 await f.list.remove('a');
 await f.list.removeFolder(f.group);
 assert.deepEqual(f.ids(), ['a']);
 await f.answer('a');
 await f.answer('a:mini');
 await pending;
 assert.ok(f.library.chat('b'));
});

test('missing records and imported mini-ID collisions never delete another session', async () => {
 const f = await setup();
 await f.chat.remove('missing');
 f.library.chats.push({ id: 'a:mini', folder: '/elsewhere' });
 await f.list.remove('a');
 await assert.rejects(f.chat.remove('a:mini'), /belongs to another chat/);
 assert.deepEqual(f.ids(), []);
 assert.equal(f.errors.length, 1);
 assert.ok(f.library.chat('a'));
 assert.ok(f.library.chat('a:mini'));
});

test('new work cannot recreate a session while its deletion awaits ACK', async () => {
 const f = await setup();
 f.chat.active = f.selected;
 const pending = f.list.remove('a');
 assert.equal(f.chat.send('new message'), false);
 assert.equal(f.chat.canCompact, false);
 f.chat.retry(f.selected, {});
 f.chat.switchModel('new-model');
 assert.deepEqual(f.ids(), ['a']);
 await f.answer('a', 'Try again later');
 await pending;
 assert.equal(f.selected.deleting, false, 'a failed deletion releases the chat for use and retry');
});

test('a chat added while folder deletion awaits ACK is not silently removed', async () => {
 const f = await setup();
 const pending = f.list.removeFolder(f.group);
 f.library.chats.push({ id: 'new', folder: '/project' });
 for (const id of ['a', 'a:mini', 'b', 'b:mini']) await f.answer(id);
 await pending;
 assert.deepEqual(f.ids(), ['a', 'a:mini', 'b', 'b:mini']);
 assert.ok(f.library.chat('new'));
 assert.ok(f.library.folders.some(folder => folder.path === '/project'));
 assert.equal(f.errors.length, 1);
});

test('successful home-chat deletion still releases its workspace; an empty folder needs no backend operation', async () => {
 const f = await setup();
 Object.assign(f.library.chat('a'), { folder: '/Chats', space: 'My chat' });
 f.chat.active = f.selected;
 const pending = f.list.remove('a');
 await f.answer('a');
 await f.answer('a:mini');
 await pending;
 assert.deepEqual(f.released, ['/Chats/My chat']);
 assert.equal(f.chat.active.folder, null);
 f.window.Backend.close({ state: 'none' });
 f.library.folders.push({ path: '/empty' });
 await f.list.removeFolder({ path: '/empty', section: node() });
 assert.equal(f.library.folders.some(folder => folder.path === '/empty'), false);
 assert.deepEqual(f.errors, []);
});
