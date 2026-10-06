// Optional bridge unit check: `node tests/pi_retry_bridge.mjs` (or bun).
// No Pi credentials/provider/runtime. Mock only the extension API; execute the
// production retry handler and context filter. Not part of the native build.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = (await readFile(new URL('../src/backend/pi/openghost-bridge.js', import.meta.url), 'utf8'))
  .replace('import { SettingsManager } from "@earendil-works/pi-coding-agent";',
           'const SettingsManager = {}; // auth is outside this test');
const { default: install } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
const handlers = new Map();
const sent = [];
let command;
install({
  on(name, handler) { handlers.set(name, handler); },
  registerCommand(name, value) { assert.equal(name, 'openghost'); command = value.handler; },
  sendMessage(message, options) { sent.push({ message, options }); },
});
const mark = (turn) => ({ type: 'custom', customType: 'openghost-turn', data: { event: 'start', turn } });
const failed = { type: 'message', id: 'failed-message', message: { role: 'assistant', stopReason: 'error' } };
let branch = [mark('failed-turn'), failed, mark('retry-turn')];
let idle = true;
let reply;
const ctx = {
  modelRegistry: {},
  isIdle: () => idle,
  sessionManager: { getBranch: () => branch },
  ui: { setStatus(key, text) { assert.equal(key, 'openghost:test'); reply = JSON.parse(text); } },
};
const retry = () => command(JSON.stringify({ op: 'retry', token: 'test', failedTurnId: 'failed-turn' }), ctx);
await retry();
assert.equal(reply.ok, true);
assert.equal(sent.length, 1);
assert.equal(sent[0].message.customType, 'openghost-retry');
assert.deepEqual(sent[0].message.content, []);
assert.equal(sent[0].options.triggerTurn, true);

branch = [mark('different-turn'), failed, mark('retry-turn')];
await retry();
assert.equal(reply.ok, false);
assert.match(reply.error, /requested turn/);
assert.equal(sent.length, 1);
for (const type of ['compaction', 'branch_summary', 'context_edit']) {
  branch = [mark('failed-turn'), failed, { type }, mark('retry-turn')];
  await retry();
  assert.equal(reply.ok, false);
  assert.match(reply.error, /context changed/);
}
branch = [mark('failed-turn'), { ...failed, message: { role: 'assistant', stopReason: 'stop' } }];
await retry();
assert.equal(reply.ok, false);
idle = false;
await retry();
assert.equal(reply.ok, false);
assert.equal(sent.length, 1);

const input = { role: 'user', content: 'original input' };
const toolCall = { role: 'assistant', stopReason: 'toolUse', content: ['tool call'] };
const toolResult = { role: 'toolResult', content: ['already executed'] };
const error = failed.message;
const trigger = { role: 'custom', customType: 'openghost-retry', content: [] };
const context = [input, toolCall, toolResult];
const filtered = handlers.get('context')({ messages: [...context, error, trigger, error, trigger] });
assert.deepEqual(filtered.messages, context); // no repeated input or removed tool effects
console.log('Pi Retry bridge: admission identity, refusal and context checks passed.');
