'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { renderer, tick } = require('./helpers');

function ledger(saved = null, written = {}) {
 const store = { read: async () => saved, write: async (key, value) => { written[key] = value; } };
 const window = renderer(['usage.js'], { ChatStore: store });
 return window.Usage;
}

test('usage events are counted in ABP terms, for any provider the backend names', async () => {
 const usage = ledger();
 await usage.ready;
 usage.record({ provider: 'openai-codex', model: 'gpt-x', modelName: 'GPT X', input: 100, cached: 400, written: 5, output: 20, requests: 2 });
 usage.record({ provider: 'test', model: 'm1', input: 10, output: 1 });
 await tick();
 const totals = usage.totals(1);
 assert.equal(totals['openai-codex'].input, 100);
 assert.equal(totals['openai-codex'].cached, 100, 'cache reads never count for more than was sent');
 assert.equal(totals['openai-codex'].requests, 2);
 assert.equal(totals.test.tokens, 11);
 assert.equal(usage.nameOf(Object.keys(totals['openai-codex'].models)[0]), 'GPT X');
});

test('reserved provider IDs have independent totals and chart buckets, never prototype properties', async () => {
 const usage = ledger();
 await usage.ready;
 const ids = ['__proto__', 'constructor', 'toString', 'a " <provider>'];
 for (const provider of ids) usage.record({ provider, model: 'm', input: 5, output: 2 });
 await tick();
 const totals = usage.totals(), day = usage.daily(1)[0];
 assert.equal(Object.getPrototypeOf(totals), null);
 assert.equal(Object.getPrototypeOf(day.providers), null);
 for (const id of ids) {
  assert.equal(totals[id].tokens, 7);
  assert.equal(day.providers[id], 7);
 }
 assert.equal(Object.prototype.tokens, undefined);
 usage.flush();
});

test('an event that counts nothing, or names no model, is left out', async () => {
 const usage = ledger();
 await usage.ready;
 usage.record({ provider: 'p', model: 'm', input: 0, output: 0 });
 usage.record({ provider: 'p', input: 5, output: 5 });
 await tick();
 assert.equal(Object.keys(usage.totals(0)).length, 0);
 assert.equal(usage.parts(null), null);
});

test('provider and model IDs containing `|` are counted whole and never collide', async () => {
 const written = {}, usage = ledger(null, written);
 await usage.ready;
 usage.record({ provider: 'a|b', model: 'c', modelName: 'First', input: 1, output: 0 });
 usage.record({ provider: 'a', model: 'b|c', modelName: 'Second', input: 20, output: 0 });
 usage.record({ provider: 'a|', model: '|b|c', input: 300, output: 0 });
 await tick();
 const totals = usage.totals(), day = usage.daily(1)[0];
 assert.deepEqual(Object.keys(totals).sort(), ['a', 'a|', 'a|b']);
 assert.equal(totals['a|b'].tokens, 1);
 assert.equal(totals.a.tokens, 20);
 assert.equal(totals['a|'].tokens, 300);
 assert.equal(day.providers['a|b'], 1);
 assert.equal(day.providers.a, 20);
 assert.equal(usage.nameOf(Object.keys(totals['a|b'].models)[0]), 'First');
 assert.equal(usage.nameOf(Object.keys(totals.a.models)[0]), 'Second');
 assert.equal(usage.nameOf(Object.keys(totals['a|'].models)[0]), '|b|c', 'an unnamed model shows its whole ID');
 usage.flush();
 await tick();
 const reopened = ledger(JSON.parse(JSON.stringify(written.usage)));
 await reopened.ready;
 const again = reopened.totals();
 assert.deepEqual(Object.keys(again).sort(), ['a', 'a|', 'a|b']);
 assert.equal(again['a|b'].tokens, 1);
 assert.equal(again.a.tokens, 20);
});

test('version 1 usage keyed `provider|model` is kept, and a model ID with `|` still reads back whole', async () => {
 const today = new Date(), pad = n => String(n).padStart(2, '0');
 const day = `${today.getFullYear()}-${pad(today.getMonth() + 1)}-${pad(today.getDate())}`;
 const saved = { version: 1, since: 5, days: { [day]: { 'openai|gpt|x': [10, 2, 1, 3, 1], 'deepseek|chat': [4, 0, 0, 1, 2] } }, names: { 'openai|gpt|x': 'GPT X' } };
 const written = {}, usage = ledger(saved, written);
 await usage.ready;
 const totals = usage.totals();
 assert.equal(usage.since, 5);
 assert.equal(totals.openai.tokens, 13);
 assert.equal(totals.openai.cached, 2);
 assert.equal(totals.deepseek.requests, 2);
 assert.equal(usage.nameOf(Object.keys(totals.openai.models)[0]), 'GPT X');
 assert.equal(usage.nameOf(Object.keys(totals.deepseek.models)[0]), 'chat');
 usage.record({ provider: 'openai', model: 'gpt|x', input: 5, output: 0 });
 await tick();
 assert.equal(usage.totals().openai.tokens, 18, 'new events add to the upgraded row');
 usage.flush();
 await tick();
 assert.equal(written.usage.version, 2);
});
