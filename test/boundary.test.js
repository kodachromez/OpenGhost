'use strict';

// The app is a frontend only: nothing in it may reach an AI provider, hold a provider's key, run the old agent or its
// tools, or build a model's prompt. Everything goes through backend-client.js to an external backend.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { ROOT } = require('./helpers');

const read = file => fs.readFileSync(path.join(ROOT, file), 'utf8');
const sources = () => [
 ...fs.readdirSync(ROOT).filter(file => file.endsWith('.js')),
 ...fs.readdirSync(path.join(ROOT, 'desktop')).filter(file => file.endsWith('.js')).map(file => `desktop/${file}`),
 'index.html', 'desktop/pdf.html',
];

const REMOVED = [
 'agent-tools.js', 'agent-prompt.js', 'providers.js', 'deepseek.js',
 'desktop/llm.js', 'desktop/openai.js', 'desktop/anthropic.js', 'desktop/chatgpt.js', 'desktop/keys.js', 'desktop/tools.js', 'desktop/media.js',
];

test('the backend files are gone', () => {
 for (const file of REMOVED) assert.equal(fs.existsSync(path.join(ROOT, file)), false, file);
});

test('no source names a provider API, an SDK or a sign-in server', () => {
 const banned = [
  /api\.deepseek\.com/, /api\.openai\.com/, /api\.anthropic\.com/, /chatgpt\.com/, /auth\.openai\.com/, /generativelanguage/,
  /@anthropic-ai/, /\bopenai\b.*require|require\(['"]openai/, /\/v1\/(chat\/completions|messages|responses)\b/, /localhost:1455/,
 ];
 for (const file of sources()) {
  const text = read(file);
  for (const pattern of banned) assert.equal(pattern.test(text), false, `${file} matches ${pattern}`);
 }
});

test('the old agent globals and prompts are gone from the renderer', () => {
 const banned = [/\bAgentTools\b/, /\bAgentPrompt\b/, /\bProviders\./, /\bDeepSeek\./, /\bTOOL_NOTES\b/, /\bFORMAT_GUIDE\b/, /\bVISUAL_CHECK\b/, /\bTITLE_PROMPT\b/, /\bCOMPACT\.prompt\b/,
  /openghost\.(llm|keys|auth|tools)\b/, /openghost\?\.(llm|keys|auth|tools)\b/];
 for (const file of sources()) {
  const text = read(file);
  for (const pattern of banned) assert.equal(pattern.test(text), false, `${file} matches ${pattern}`);
 }
});

test('the renderer makes no network requests and its CSP allows none', () => {
 for (const file of sources().filter(file => !file.startsWith('desktop/'))) {
  assert.equal(/\bfetch\(|XMLHttpRequest|new WebSocket|EventSource|sendBeacon/.test(read(file)), false, file);
 }
 const csp = read('index.html').match(/Content-Security-Policy" content="([^"]+)"/)[1];
 assert.match(csp, /connect-src 'none'/);
});

test('only the backend host starts processes, and the main process fetches only YouTube oEmbed', () => {
 for (const file of sources().filter(file => file.startsWith('desktop/'))) {
  const text = read(file);
  if (file !== 'desktop/backend-host.js') assert.equal(/child_process/.test(text), false, file);
  const fetches = text.match(/net\.fetch\([^)]*/g) || [];
  for (const call of fetches) assert.equal(file, 'desktop/main.js', `${file}: ${call}`);
 }
 const main = read('desktop/main.js');
 assert.match(main, /https:\/\/www\.youtube\.com\/oembed\?url=/);
 assert.equal((main.match(/net\.fetch\(/g) || []).length, 1);
});

test('the preload exposes the backend bridge and none of the old backend channels', () => {
 const preload = read('desktop/preload.js');
 for (const channel of ['llm:', 'keys:', 'auth:', 'tool:']) assert.equal(preload.includes(`'${channel}`), false, channel);
 for (const channel of ['backend:send', 'backend:message', 'backend:status']) assert.ok(preload.includes(channel), channel);
});

test('every script the page loads exists, and the boundary loads before the UI that uses it', () => {
 const html = read('index.html');
 const scripts = [...html.matchAll(/<script src="([^"]+)"/g)].map(match => match[1]);
 for (const script of scripts) assert.ok(fs.existsSync(path.join(ROOT, script)), script);
 for (const gone of REMOVED) assert.equal(scripts.includes(gone), false, gone);
 const at = name => scripts.indexOf(name);
 assert.ok(at('backend-protocol.js') >= 0 && at('backend-protocol.js') < at('backend-client.js'));
 for (const user of ['settings.js', 'chat.js', 'host-tools.js']) assert.ok(at('backend-client.js') < at(user), user);
 assert.ok(at('host-tools.js') < at('chat.js'));
});

test('the package has no runtime dependencies and does not ship the tests or docs', () => {
 const pkg = JSON.parse(read('package.json'));
 assert.equal(pkg.dependencies, undefined);
 assert.ok(pkg.build.files.includes('!test/**'));
 assert.ok(pkg.build.files.includes('!docs/**'));
 assert.equal(read('package-lock.json').includes('@anthropic-ai/sdk'), false);
});

test('every I18n key the app names exists', () => {
 const i18n = read('i18n.js');
 const missing = [];
 for (const file of sources()) {
  for (const match of read(file).matchAll(/I18n\.t\('([\w.-]+)'/g)) if (!i18n.includes(`'${match[1]}':`)) missing.push(`${file}: ${match[1]}`);
 }
 assert.deepEqual(missing, []);
});
