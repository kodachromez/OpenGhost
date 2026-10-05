(() => {
'use strict';

// The app's own count of the tokens each provider was sent and wrote back, kept by day and model on this computer only.
// The numbers come from the backend's `usage` events, already in the same words for every provider (ABP v0). The count
// starts with the version that brought it: nothing was kept before.
const KEY = 'usage';
const SAVE_DELAY = 800;
// Per day and model: tokens sent, of them read from the provider's cache, written to it, tokens written back, requests.
const [INPUT, CACHED, WRITTEN, OUTPUT, REQUESTS] = [0, 1, 2, 3, 4];

const pad = n => String(n).padStart(2, '0');
const dayOf = date => `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`;

// The day `back` days before today, by the calendar rather than by hours, so a clock change never skips one.
function daysAgo(back) {
 const date = new Date();
 date.setDate(date.getDate() - back);
 return dayOf(date);
}

// Each model is kept under its provider and model IDs written as a JSON pair, so any ID, `|` and all, reads back whole.
// Version 1 wrote `provider|model`; its provider IDs never held a `|`, so the first one splits such a key.
const keyOf = (provider, model) => JSON.stringify([provider, model]);
function idOf(key) {
 try {
  const id = JSON.parse(key);
  if (Array.isArray(id) && id.length === 2 && id.every(part => typeof part === 'string')) return id;
 } catch {}
 return null;
}
function rekey(rows) {
 const out = {};
 for (const [key, value] of Object.entries(rows || {})) {
  const cut = key.indexOf('|');
  if (cut > 0) out[keyOf(key.slice(0, cut), key.slice(cut + 1))] = value;
 }
 return out;
}
function upgrade(saved) {
 const days = {};
 for (const [day, rows] of Object.entries(saved.days || {})) days[day] = rekey(rows);
 return { ...saved, version: 2, days, names: rekey(saved.names) };
}

const empty = () => ({ input: 0, cached: 0, written: 0, output: 0, requests: 0, tokens: 0, models: {} });

class Usage {
 constructor(store) {
  this.store = store;
  this.data = { version: 2, since: 0, days: {}, names: {} };
  this.listeners = new Set();
  this.timer = 0;
  this.ready = store.read(KEY).then(saved => {
   if (saved?.version === 1) this.data = { ...this.data, ...upgrade(saved) };
   else if (saved?.version === 2) this.data = { ...this.data, ...saved };
  }).catch(() => {});
 }

 // One usage event's tokens: sent, of them read from the provider's cache or written to it, and written back. Null when
 // it counts nothing.
 parts(usage) {
  const count = value => Math.max(0, Number(value) || 0);
  const input = count(usage?.input), output = count(usage?.output);
  if (!input && !output) return null;
  return { input, cached: Math.min(count(usage.cached), input), written: count(usage.written), output, requests: Math.max(1, count(usage.requests)) };
 }

 // One usage event from the backend: what was sent (and how much of it the provider read from its cache or wrote to it)
 // and what came back, by provider and model.
 record(usage) {
  const parts = this.parts(usage), provider = String(usage?.provider || ''), model = String(usage?.model || ''), name = usage?.modelName;
  if (!parts || !provider || !model) return;
  const { input, cached, written, output, requests } = parts;
  this.ready.then(() => {
   const data = this.data, id = keyOf(provider, model);
   data.since ||= Date.now();
   const row = (data.days[daysAgo(0)] ||= {})[id] ||= [0, 0, 0, 0, 0];
   row[INPUT] += input;
   row[CACHED] += cached;
   row[WRITTEN] += written;
   row[OUTPUT] += output;
   row[REQUESTS] += requests;
   if (name) data.names[id] = name;
   this.save();
   for (const listener of this.listeners) listener();
  });
 }

 // Every provider's tokens over the last `days` days, today included; with no count, over all the time there is.
 totals(days = 0) {
  return this.between(days ? daysAgo(days - 1) : '', '9999');
 }

 // Every provider's tokens from one day to another, both included; days are written YYYY-MM-DD.
 between(from, to) {
  const out = Object.create(null);
  for (const [day, rows] of Object.entries(this.data.days)) {
   if (day < from || day > to) continue;
   for (const [id, row] of Object.entries(rows)) {
    const provider = idOf(id)?.[0];
    if (provider === undefined) continue;
    const total = out[provider] ||= empty();
    total.input += row[INPUT];
    total.cached += row[CACHED];
    total.written += row[WRITTEN];
    total.output += row[OUTPUT];
    total.requests += row[REQUESTS];
    total.tokens += row[INPUT] + row[OUTPUT];
    total.models[id] = (total.models[id] || 0) + row[INPUT] + row[OUTPUT];
   }
  }
  return out;
 }

 // The last `count` days, oldest first, each with the tokens of every provider that day.
 daily(count) {
  return Array.from({ length: count }, (_, k) => this.day(daysAgo(count - 1 - k)));
 }

 // Every day of a month, written YYYY-MM, the days still to come included.
 month(key) {
  const [year, month] = key.split('-').map(Number), length = new Date(year, month, 0).getDate();
  return Array.from({ length }, (_, k) => this.day(`${key}-${pad(k + 1)}`));
 }

 day(day) {
  const rows = this.data.days[day] || {}, providers = Object.create(null);
  for (const [id, row] of Object.entries(rows)) {
   const provider = idOf(id)?.[0];
   if (provider === undefined) continue;
   providers[provider] = (providers[provider] || 0) + row[INPUT] + row[OUTPUT];
  }
  return { day, providers };
 }

 // The months with anything counted, oldest first, written YYYY-MM.
 months() {
  return [...new Set(Object.keys(this.data.days).map(day => day.slice(0, 7)))].sort();
 }

 // This month, written YYYY-MM.
 get thisMonth() {
  return daysAgo(0).slice(0, 7);
 }

 nameOf(id) {
  return this.data.names[id] || (idOf(id)?.[1] ?? id);
 }

 get since() {
  return this.data.since;
 }

 onChange(listener) {
  this.listeners.add(listener);
 }

 save() {
  clearTimeout(this.timer);
  this.timer = setTimeout(() => {
   this.timer = 0;
   this.store.write(KEY, this.data).catch(() => {});
  }, SAVE_DELAY);
 }

 flush() {
  if (!this.timer) return;
  clearTimeout(this.timer);
  this.timer = 0;
  this.store.write(KEY, this.data).catch(() => {});
 }
}

window.Usage = new Usage(window.ChatStore);
})();
