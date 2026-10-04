'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { renderer, tick } = require('./helpers');

function ledger() {
 const written = {};
 const store = { read: async () => null, write: async (key, value) => { written[key] = value; } };
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
 assert.equal(usage.nameOf('openai-codex|gpt-x'), 'GPT X');
});

test('an event that counts nothing, or names no model, is left out', async () => {
 const usage = ledger();
 await usage.ready;
 usage.record({ provider: 'p', model: 'm', input: 0, output: 0 });
 usage.record({ provider: 'p', input: 5, output: 5 });
 usage.record({ provider: 'a|b', model: 'm', input: 5, output: 5 });
 await tick();
 assert.equal(Object.keys(usage.totals(0)).length, 0);
 assert.equal(usage.parts(null), null);
});
