'use strict';

// F18: legacy compatibility ends at the display-store boundary, not in a model/provider runtime.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { ROOT, renderer } = require('./helpers');
const plain = value => JSON.parse(JSON.stringify(value));
const IMAGE = 'data:image/png;base64,aGVsbG8=';
const usage = { input: 12, cached: 3, written: 2, output: 7, requests: 1 };
const stats = { version: 1, models: [{ id: 'old:model', name: 'Old model', ...usage }], turns: [{ m: 0, t: 19, c: 3 }],
 mini: { ...usage }, context: { used: 20, window: 100 }, uncounted: 1 };
const legacy = [
 { role: 'user', text: 'Look at this', content: [{ type: 'text', text: 'MODEL_ONLY attachment XML' }, { type: 'image_url', image_url: { url: IMAGE } }],
  attachments: [{ name: 'photo.png', size: 5, image: true, width: 100, height: 50 },
   { name: 'clip.mp4', video: { path: '/clip.mp4', poster: IMAGE, duration: 3 } },
   { name: 'paste.txt', pasted: { preview: 'first line', lines: 5 }, size: 90 }], cache: true },
 { role: 'assistant', content: '**Visible answer**\n```mermaid\nmetrics\nCount | 2\n```', turn: 'old', model: 'old:model',
  steps: [{ role: 'tool', content: 'MODEL_ONLY tool result' }], native: { thinking: 'MODEL_ONLY' }, reasoning_content: 'MODEL_ONLY', cache: true },
 { role: 'assistant', content: 'Counted answer', turn: 'counted', model: 'old:model', usage, steps: [{ native: 'MODEL_ONLY' }] },
 { role: 'compact', content: 'MODEL_ONLY summary', model: 'old:model', usage },
 { role: 'stats', stats }, { role: 'moved' },
 { role: 'system', content: 'MODEL_ONLY instructions' }, { role: 'tool', content: 'MODEL_ONLY' },
];

function node() {
 return { children: [], classList: { add() {}, remove() {} },
  append(...items) { this.children.push(...items); }, getAnimations: () => [], remove() {}, addEventListener() {} };
}

async function fixture(messages = legacy) {
 const data = new Map([
  ['index', { chats: [{ id: 'c', title: 'Old chat', folder: '/work' }], folders: [] }],
  ['chats/c', { version: 1, messages: plain(messages), tokens: 20 }],
  ['mini/c', { version: 1, messages: plain(messages), tokens: 10, seen: 123 }],
 ]);
 const calls = [];
 const window = vm.createContext({ console, setTimeout, clearTimeout, structuredClone, TextEncoder, TextDecoder, btoa, atob, crypto: webcrypto,
  document: { createElement: node }, I18n: { t: key => key },
  BackendError: class extends Error { constructor(error) { super(error.message); } },
  Backend: { on() {}, handle() {}, ready: Promise.resolve(), available: true, can: () => true,
   async request(method, params) { calls.push({ method, params }); return { exists: false }; }, explain: error => error },
  FileKinds: { describe: name => ({ name }) }, Glyphs: { quote: '' }, LinkChip: { fill: (el, text) => { el.textContent = text; } },
  StreamView: { render: (el, text) => { assert.equal(typeof text, 'string'); el.rendered = text; } },
  StatsCard: { build: value => ({ stats: value }) },
 });
 window.window = window;
 for (const file of ['chat-lock.js', 'library.js', 'chat.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), window, { filename: file });
 const library = new window.Library({ read: async key => plain(data.get(key) || null), write: async (key, value) => { data.set(key, plain(value)); } }, () => {});
 await library.ready;
 const chat = Object.assign(Object.create(window.Chat.prototype), { library, active: null, conversations: new Map(), nodes: new Map(), opening: 0,
  settings: { find: () => null, resolve: () => 'old:model', windowOf: () => 100 }, onChange() {}, attach() {}, activate(conv) { this.active = conv; },
  toolbar: node, compactNotice: () => ({ compact: true }),
  // Exercise real open/load/restore/entryView/promptOf; only media/DOM painting is stubbed.
  attachmentViews: attachments => attachments.map(attachment => ({ attachment })),
 });
 return { window, library, chat, data, calls };
}

function displayOnly(messages) {
 const serialized = JSON.stringify(messages);
 assert.doesNotMatch(serialized, /MODEL_ONLY|"(?:steps|native|cache|reasoning_content|image_url|tool_calls|tool_call_id|payload|secret|path)"/);
}

test('render guide describes syntax/capabilities without agent-behavior policy', () => {
 const { RenderGuide: guide } = renderer(['render-guide.js']);
 for (const syntax of ['Markdown', '##', '**bold**', 'mermaid', 'flowchart', 'sequenceDiagram', 'xychart-beta', 'metrics', 'wireframe mobile',
  'name | size | modified', 'more <number>', '![caption]', 'youtube.com', '\\( … \\)', '[!WARNING]', 'worksheet', 'style and classDef directives are ignored']) {
  assert.ok(guide.includes(syntax), syntax);
 }
 assert.doesNotMatch(guide, /\b(?:must|never|always|should|prefer|first choice)\b/i);
 assert.doesNotMatch(guide, /split longer answers|keep headings short|text-only answer|carries several drawings|whenever something|realistic illustrative data|every such address|from memory|choose the form|by what the reader needs|at most 30|say only what stands out|language of the answer/i);
});

test('old saved chats open for display without a backend session, preserving text, media, markers and stats', async () => {
 const { chat, library, data, calls } = await fixture();
 await chat.open('c');
 const conv = chat.active;
 assert.equal(conv.reconciled, false);
 assert.match(conv.recoveryNotice.textContent, /session is missing/);
 assert.deepEqual(calls.map(call => call.method), ['session.get'], 'no legacy history import or provider request');
 assert.equal(conv.messages.length, 6);
 assert.equal(conv.messages[0].text, 'Look at this');
 assert.equal(conv.messages[0].attachments[0].url, IMAGE);
 const prompt = chat.promptOf(conv.messages[0]);
 assert.equal(prompt.attachments[1].url, IMAGE);
 assert.equal(prompt.attachments[1].duration, 3);
 assert.equal(prompt.attachments[2].pasted.preview, 'first line');
 assert.equal(conv.list.children[0].children[3].textContent, 'Look at this');
 assert.equal(conv.list.children[1].children[0].rendered, legacy[1].content);
 assert.equal(conv.list.children[3].compact, true);
 assert.deepEqual(plain(conv.list.children[4].stats), stats);
 assert.equal(conv.messages[1].uncounted, true);
 assert.deepEqual(plain(conv.messages[2].usage), usage);
 assert.equal((await chat.stats(conv)).uncounted, 1);
 displayOnly(conv.messages);
 const side = await library.side('c');
 assert.deepEqual(plain(side.messages), plain(conv.messages));
 assert.equal(side.seen, 123);
 assert.deepEqual(data.get('chats/c').messages, legacy, 'opening alone does not rewrite the original');
});

test('malformed legacy image/model-shaped data is discarded without throwing or shifting image slots', async () => {
 const malformed = [null, 4, [], { role: 'tool', content: {} },
  { role: 'user', text: 'bad images', content: [null, 5, { type: 'image_url' }, { type: 'image_url', image_url: null },
   { type: 'image_url', image_url: { url: {} } }, { type: 'image_url', image_url: { url: IMAGE } }],
   attachments: [null, 5, { name: {} }, ...['missing', 'null', 'object', 'valid'].map(name => ({ name, image: true, url: 42 }))] },
  { role: 'user', text: {}, content: [{ type: 'text', text: 'MODEL_ONLY' }], attachments: {} },
  { role: 'assistant', content: [{ type: 'thinking', thinking: 'MODEL_ONLY' }], model: {}, usage: [], steps: { length: 3 } },
  { role: 'assistant', content: {}, usage: { input: {}, output: 'bad', native: 'MODEL_ONLY' } },
  { role: 'stats', stats: { models: [null, { name: {}, input: 'bad' }], turns: [null, {}], context: { used: {} } } },
  { role: 'stats', stats: null },
 ];
 const { chat, library } = await fixture(malformed);
 await chat.open('c');
 const messages = chat.active.messages;
 assert.deepEqual(plain(chat.promptOf(messages[0]).attachments.map(item => item.url)), ['', '', '', IMAGE]);
 assert.equal(messages[1].text, '');
 assert.equal(messages[2].content, '');
 assert.equal(messages[2].uncounted, undefined);
 assert.equal(messages[3].content, '');
 displayOnly(messages);
 await library.saveMessages('c', malformed);
 assert.deepEqual(plain((await library.conversation('c')).messages), plain(messages));
 for (const value of [null, {}, 'bad']) {
  await library.saveMessages('c', value);
  assert.deepEqual(plain((await library.conversation('c')).messages), []);
 }
});

test('new main/mini saves project nested display fields and retain required recovery IDs only', async () => {
 const { library, data } = await fixture();
 const input = plain(legacy);
 Object.assign(input[0], { backendTurn: 'pending', clientInputId: 'steer', pendingTurn: true, inputError: 'Not confirmed', tool_calls: ['MODEL_ONLY'] });
 input[0].attachments[0].payload = { secret: 'MODEL_ONLY' };
 input[0].attachments[1].video.native = 'MODEL_ONLY';
 input[0].attachments[2].pasted.text = 'MODEL_ONLY';
 input[2].usage.native = 'MODEL_ONLY';
 input[4].stats.models[0].secret = 'MODEL_ONLY';
 input[4].stats.turns[0].native = 'MODEL_ONLY';
 const before = plain(input);
 await library.saveMessages('c', input, 20);
 await library.saveSide('c', { messages: input, tokens: 10, seen: 123 });
 const main = data.get('chats/c'), mini = data.get('mini/c');
 displayOnly(main.messages);
 assert.deepEqual(main.messages, mini.messages);
 assert.equal(main.messages[0].backendTurn, 'pending');
 assert.equal(main.messages[0].clientInputId, 'steer');
 assert.equal(main.messages[0].pendingTurn, true);
 assert.equal(main.messages[0].inputError, 'Not confirmed');
 assert.deepEqual(main.messages[4].stats, stats);
 assert.equal(main.tokens, 20);
 assert.equal(mini.seen, 123);
 assert.deepEqual(input, before, 'projection never mutates live entries');
 await library.saveMessages('c', (await library.conversation('c')).messages, 20);
 assert.deepEqual(data.get('chats/c'), main, 'projection is idempotent');
});

test('normal display saves preserve markdown, previews, usage and annotations', async () => {
 const current = [
  { role: 'user', text: 'hello', content: 'hello', attachments: [{ name: 'image.png', image: true, url: IMAGE, width: 50, height: 20, note: 'caption' }] },
  { role: 'assistant', content: '# Answer\n\\(x\\)\n```mermaid\nfiles\nreport.txt | 5 B\n```', model: 'm', turn: 't', backendTurn: 't', usage },
  { role: 'compact', model: 'm', usage }, { role: 'stats', stats }, { role: 'moved' },
 ];
 const { library, data } = await fixture(current);
 assert.deepEqual(plain((await library.conversation('c')).messages), current);
 await library.saveMessages('c', current);
 assert.deepEqual(data.get('chats/c').messages, current);
});

test('sealed chats and lock transitions use the same projection, including the mini chat', async () => {
 const { window, library, data } = await fixture();
 await library.protect('c', 'password', { messages: legacy, tokens: 20 });
 await assert.rejects(library.conversation('c'), 'locked chats must not become empty writable chats');
 assert.equal(await library.unlock('c', 'password'), true);
 const readSealed = async key => window.ChatLock.open(library.keys.get('c'), data.get(key).sealed);
 displayOnly((await readSealed('chats/c')).messages);
 displayOnly((await readSealed('mini/c')).messages);
 // Simulate pre-migration encrypted files as well, rather than only files sealed by this build.
 const oldSealed = await window.ChatLock.seal(library.keys.get('c'), { messages: legacy, tokens: 20, seen: 123 });
 data.set('chats/c', { version: 1, sealed: oldSealed });
 data.set('mini/c', { version: 1, sealed: oldSealed });
 const opened = await library.conversation('c');
 assert.equal(opened.messages[0].attachments[0].url, IMAGE);
 displayOnly(opened.messages);
 displayOnly((await library.side('c')).messages);
 await library.saveMessages('c', legacy, 20);
 await library.saveSide('c', { messages: legacy, tokens: 10, seen: 123 });
 displayOnly((await readSealed('chats/c')).messages);
 displayOnly((await readSealed('mini/c')).messages);
 await library.unprotect('c', { messages: legacy, tokens: 20 });
 displayOnly(data.get('chats/c').messages);
 displayOnly(data.get('mini/c').messages);
 assert.equal(data.get('mini/c').seen, 123);
});
