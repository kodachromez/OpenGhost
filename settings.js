(() => {
'use strict';

const STORAGE = { effort: 'deepseek.effort', mode: 'openghost.mode', model: 'openghost.model', catalog: 'openghost.catalog' };
// How long the backend's list of models counts as fresh. Opening the model picker after that reads the list again.
const FRESH = 10 * 60 * 1000;
const MODES = ['ask', 'auto', 'full'];
const DEFAULT_MODE = 'ask';
const CHECK_DELAY = 400;
const PAGE = { duration: 460, easing: 'cubic-bezier(0.32, 0.72, 0, 1)' };

const reducedMotion = () => window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const escapeHtml = text => String(text).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const hostOf = url => { try { return new URL(url).host; } catch { return ''; } };

// Providers, their sign-in methods and their models all come from the backend (auth.providers, models.list); the app
// knows none by itself. A provider's section is drawn from what the backend says about it, in the same rows as ever:
// a field for an API key, a row to sign in with an account.
function keyRow(index, method) {
 const link = method.url && /^https:\/\//.test(method.url) ? ` <a href="${escapeHtml(method.url)}" target="_blank" rel="noopener noreferrer">${escapeHtml(hostOf(method.url))}</a>.` : '';
 return `
  <div class="settings-row">
   <div class="settings-text">
    <label class="settings-label" for="settings-key-${index}">${escapeHtml(method.label || I18n.t('settings.key.label'))}</label>
    <p class="settings-hint"><span>${escapeHtml(method.hint || '')}</span>${link}</p>
   </div>
   <div class="settings-control">
    <div class="settings-key-box">
     <input id="settings-key-${index}" class="settings-key" type="password" placeholder="${escapeHtml(method.placeholder || 'sk-…')}" autocomplete="off" spellcheck="false">
     <button type="button" class="settings-key-eye" aria-label="${escapeHtml(I18n.t('settings.key.show'))}" aria-pressed="false">${Glyphs.eye}</button>
    </div>
    <p class="settings-status" role="status"></p>
   </div>
  </div>`;
}

function accountRow(method) {
 return `
  <div class="settings-row">
   <div class="settings-text">
    <span class="settings-label">${escapeHtml(method.label || '')}</span>
    <p class="settings-hint">${escapeHtml(method.hint || '')}</p>
   </div>
   <div class="settings-control settings-account">
    <div class="settings-account-row">
     <span class="settings-account-who"></span>
     <button type="button" class="settings-button is-primary" data-action="login">${escapeHtml(method.action || I18n.t('settings.account.login'))}</button>
     <button type="button" class="settings-button" data-action="cancel">${escapeHtml(I18n.t('settings.account.cancel'))}</button>
     <button type="button" class="settings-button" data-action="logout">${escapeHtml(I18n.t('settings.account.logout'))}</button>
    </div>
    <p class="settings-status" role="status"></p>
   </div>
  </div>`;
}

function section(index, name, rows) {
 return `
  <section class="provider" aria-labelledby="settings-provider-${index}">
   <header class="provider-head">
    <h3 class="provider-name" id="settings-provider-${index}">${escapeHtml(name)}</h3>
    <span class="provider-models"></span>
    <span class="provider-state"></span>
   </header>
   ${rows}
  </section>`;
}

const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const text = value => typeof value === 'string' ? value : '';

function statusOf(value) {
 if (!record(value) || typeof value.connected !== 'boolean') return null;
 if (value.error != null && typeof value.error !== 'string' && (!record(value.error) || typeof value.error.message !== 'string')) return null;
 const error = text(value.error) || text(value.error?.message);
 return {
  connected: value.connected, checking: value.checking === true, waiting: value.waiting === true,
  keySaved: value.keySaved === true,
  account: { email: text(value.account?.email), plan: text(value.account?.plan) },
  error,
 };
}

function providersOf(values) {
 const seen = new Set();
 return values.filter(item => record(item) && typeof item.id === 'string' && !seen.has(item.id) && seen.add(item.id)).map(item => {
  // ABP v0 addresses auth by provider, not method ID: render at most one of each supported kind.
  const kinds = new Set();
  const methods = (Array.isArray(item.methods) ? item.methods : []).filter(method =>
   record(method) && ['apiKey', 'oauth'].includes(method.type) && !kinds.has(method.type) && kinds.add(method.type)
  ).map(method => Object.fromEntries(['type', 'label', 'hint', 'url', 'placeholder', 'action'].map(key => [key, text(method[key])])));
  return { id: item.id, name: text(item.name) || item.id, group: text(item.group), limits: item.limits === true, methods, status: statusOf(item.status) };
 });
}

// A model as the backend lists it (ABP Model), as the picker and the effort control use it.
const modelOf = model => ({
 id: `${model.provider}:${model.id}`,
 provider: String(model.provider),
 api: String(model.id),
 name: text(model.name) || String(model.id),
 context: Number.isFinite(model.contextWindow) && model.contextWindow > 0 ? model.contextWindow : 0,
 efforts: Array.isArray(model.thinkingLevels) ? model.thinkingLevels.filter(level => typeof level === 'string' && level) : [],
 defaultEffort: model.defaultThinking,
 vision: model.vision === true,
});

class Settings {
 constructor(dialog) {
  this.dialog = dialog;
  this.list = dialog.querySelector('.settings-providers');
  this.providers = [];
  this.status = Object.create(null);
  this.authVersions = new Map();
  this.messages = new Map();
  this.providerCheck = 0;
  this.models = this.readCatalog();
  this.efforts = [];
  localStorage.removeItem('deepseek.model');
  this.model = localStorage.getItem(STORAGE.model) || '';
  this.shown = null;
  this.read = 0;
  const effort = localStorage.getItem(STORAGE.effort);
  this.preferredEffort = effort || undefined;
  this.effort = undefined;
  const mode = localStorage.getItem(STORAGE.mode);
  this.mode = MODES.includes(mode) ? mode : DEFAULT_MODE;
  this.typed = Object.create(null);
  this.timer = Object.create(null);
  this.build();
  this.pager();
  dialog.addEventListener('dismiss', () => dialog.close());
  dialog.addEventListener('close', () => this.conceal());
  // Mid-transition of the theme a click lands on <html>, outside the dialog, and must not close it.
  dialog.addEventListener('cancel', event => {
   if (window.Theme?.moving) event.preventDefault();
  });
  // The backend says when what it offers has changed: a sign-in went through or lapsed, a provider listed new models.
  Backend.on('ready', () => this.refreshAll());
  Backend.on('closed', () => {
   ++this.providerCheck;
   this.checks = (this.checks || 0) + 1;
   this.authVersions.clear();
   this.messages.clear();
   for (const timer of Object.values(this.timer)) clearTimeout(timer);
   this.timer = Object.create(null);
   this.typed = Object.create(null);
   this.providers = [];
   this.status = Object.create(null);
   this.build();
  });
  Backend.on('auth.changed', event => {
   if (!Backend.available || typeof event?.provider !== 'string' || !statusOf(event.status)) return;
   // Events have no revision/operation ID. Re-read authoritative state, never trust a late event's snapshot.
   const token = this.authVersions.get(event.provider);
   if (token?.pending) token.changed = true;
   else this.authVersions.set(event.provider, {});
   this.refreshAll();
  });
  Backend.on('models.changed', () => this.refreshAll());
  if (Backend.available) this.refreshAll();
 }

 // The sections on the left: one highlight glides to the chosen section, and its page rises into view.
 pager() {
  const dialog = this.dialog;
  this.tabs = [...dialog.querySelectorAll('.settings-tab')];
  this.panels = Object.fromEntries([...dialog.querySelectorAll('.settings-panel')].map(panel => [panel.dataset.page, panel]));
  this.glider = dialog.querySelector('.settings-glide');
  this.title = dialog.querySelector('.settings-page-title');
  this.scroller = dialog.querySelector('.settings-page');
  this.current = 'general';
  // As in the chat, the page fades into an edge while more of it lies scrolled out past that edge.
  const edges = () => {
   const page = this.scroller;
   dialog.classList.toggle('can-up', page.scrollTop > 1);
   dialog.classList.toggle('can-down', page.scrollTop + page.clientHeight < page.scrollHeight - 1);
  };
  this.scroller.addEventListener('scroll', edges, { passive: true });
  const sizes = new ResizeObserver(edges);
  sizes.observe(this.scroller);
  for (const panel of dialog.querySelectorAll('.settings-panel')) sizes.observe(panel);
  for (const tab of this.tabs) tab.addEventListener('click', () => this.page(tab.dataset.page));
  dialog.querySelector('.settings-tabs').addEventListener('keydown', event => {
   const step = { ArrowDown: 1, ArrowUp: -1 }[event.key];
   if (!step) return;
   event.preventDefault();
   const at = this.tabs.findIndex(tab => tab.dataset.page === this.current);
   const next = this.tabs[(at + step + this.tabs.length) % this.tabs.length];
   this.page(next.dataset.page);
   next.focus();
  });
 }

 page(name, instant = false) {
  const panel = this.panels[name];
  if (!panel) return;
  const moved = name !== this.current;
  this.current = name;
  for (const tab of this.tabs) {
   const on = tab.dataset.page === name;
   tab.setAttribute('aria-selected', String(on));
   tab.tabIndex = on ? 0 : -1;
  }
  this.glide(instant || !moved);
  if (!moved) return;
  for (const item of Object.values(this.panels)) item.hidden = item !== panel;
  this.title.textContent = I18n.t(`settings.${name}`);
  this.scroller.scrollTop = 0;
  if (instant || reducedMotion()) return;
  panel.animate([{ opacity: 0, transform: 'translateY(10px)', filter: 'blur(6px)' }, { opacity: 1, transform: 'none', filter: 'blur(0)' }], PAGE);
  this.title.animate([{ opacity: 0, transform: 'translateY(3px)', filter: 'blur(4px)' }, { opacity: 1, transform: 'none', filter: 'blur(0)' }], { ...PAGE, duration: 320 });
 }

 glide(instant) {
  const tab = this.tabs.find(item => item.dataset.page === this.current);
  if (!tab || !this.dialog.open) return;
  const style = this.glider.style;
  if (instant) style.transition = 'none';
  style.transform = `translateY(${tab.offsetTop}px)`;
  style.height = `${tab.offsetHeight}px`;
  style.opacity = '1';
  if (!instant) return;
  void this.glider.offsetHeight;
  style.transition = '';
 }

 // The models the backend listed last time, so the picker has them at once while the backend starts.
 readCatalog() {
  try {
   const saved = JSON.parse(localStorage.getItem(STORAGE.catalog));
   // Older catalogs contain inferred capabilities, not just backend data.
   if (saved?.version === 3 && Array.isArray(saved.models)) return saved.models;
  } catch {}
  return [];
 }

 saveCatalog() {
  try { localStorage.setItem(STORAGE.catalog, JSON.stringify({ version: 3, models: this.models })); } catch {}
 }

 // The eye beside a key shows what was typed for a moment's check; closing the settings hides every key again. A saved
 // key is the backend's and never comes back to the app.
 reveal(provider, shown) {
  const input = this.inputs[provider], eye = input.nextElementSibling;
  input.type = shown ? 'text' : 'password';
  input.parentElement.classList.toggle('is-revealed', shown);
  eye.setAttribute('aria-pressed', String(shown));
  eye.setAttribute('aria-label', I18n.t(shown ? 'settings.key.hide' : 'settings.key.show'));
 }

 conceal() {
  for (const provider of Object.keys(this.inputs || {})) this.reveal(provider, false);
 }

 connected(provider) {
  return !!this.status[provider]?.connected;
 }

 // The badge turns green only once the backend says the provider took the key or the sign-in.
 working(provider) {
  return this.connected(provider) && !this.status[provider]?.checking;
 }

 // A provider's name as the backend gives it, and the heading its models go under in the picker.
 nameOf(provider) {
  return this.providers.find(item => item.id === provider)?.name || provider;
 }

 groupOf(provider) {
  const item = this.providers.find(entry => entry.id === provider);
  return item?.group || item?.name || provider;
 }

 // The providers in the backend's order.
 get order() {
  return this.providers.map(item => item.id);
 }

 // The picker offers the models the backend listed, in its providers' order. No model is known to the app by itself:
 // with none listed there is none, and the picker leads to the settings instead.
 collect() {
  const order = this.order, rank = provider => { const at = order.indexOf(provider); return at < 0 ? order.length : at; };
  this.models = this.models.slice().sort((a, b) => rank(a.provider) - rank(b.provider));
  this.paint();
 }

 find(id) {
  return this.models.find(item => item.id === id) || null;
 }

 // Keep unavailable selections so sending cannot silently switch models/providers. Legacy bare names still resolve
 // when unambiguous; only a chat with no selection gets the initial catalog choice.
 resolve(id) {
  const selected = id || this.model;
  if (!selected) return this.models[0]?.id || '';
  if (this.find(selected) || selected.includes(':')) return selected;
  const named = this.models.filter(item => item.api === selected);
  return named.length === 1 ? named[0].id : selected;
 }

 // What a turn tells the backend about its model: which one, and how hard it thinks. `ready` once the backend listed it.
 configFor(id) {
  const model = this.find(id);
  const efforts = model?.efforts || [];
  const effort = this.canonicalEffort && this.canonicalEffort.id === id ? this.canonicalEffort.value
   : efforts.includes(this.preferredEffort) ? this.preferredEffort
   : efforts.includes(model?.defaultEffort) ? model.defaultEffort : undefined;
  return {
   id: model?.id || id,
   provider: model?.provider || '',
   model: model?.api || id,
   name: model?.name || id,
   ready: !!model,
   effort,
   efforts,
   vision: model?.vision === true,
  };
 }

 get config() {
  return this.configFor(this.resolve(this.model));
 }

 windowOf(id) {
  return this.find(id)?.context || 0;
 }

 // The effort steps follow the model of the chat on screen.
 show(id) {
  if (id === this.shown) return;
  this.shown = id;
  this.applyEfforts();
 }

 applyEfforts() {
  const { efforts, effort } = this.configFor(this.resolve(this.shown));
  const same = effort === this.effort && efforts.length === this.efforts.length && efforts.every((level, i) => level === this.efforts[i]);
  this.efforts = efforts.slice();
  this.effort = effort;
  // Backend defaults are display state, not a saved user preference.
  if (!same) this.onEfforts?.(this.efforts);
 }

 setModel(id, canonical = false) {
  if (id === this.model || !canonical && !this.find(id)) return;
  this.model = id;
  localStorage.setItem(STORAGE.model, id);
 }

 setEffort(value, canonicalId = '') {
  value = typeof value === 'string' && value ? value : undefined;
  this.canonicalEffort = canonicalId ? { id: canonicalId, value } : null;
  this.preferredEffort = value;
  if (value == null) localStorage.removeItem(STORAGE.effort);
  else localStorage.setItem(STORAGE.effort, value);
  this.applyEfforts();
 }

 setMode(value) {
  if (!MODES.includes(value)) return;
  this.mode = value;
  localStorage.setItem(STORAGE.mode, value);
  window.ModePicker?.sync();
 }

 changed() {
  this.collect();
  this.applyEfforts();
  this.onModels?.();
 }

 // Everything the backend offers, read anew: its providers with how each stands, then its models.
 async refreshAll() {
  this.read = Date.now();
  const check = ++this.providerCheck, versions = new Map(this.authVersions);
  const pending = new Set([...versions].filter(([, token]) => token.pending).map(([id]) => id));
  const providers = Backend.can('auth.providers') ? await Backend.request('auth.providers').catch(() => null) : [];
  if (check !== this.providerCheck || !Backend.available) return;
  if (Array.isArray(providers)) {
   this.providers = providersOf(providers);
   const status = Object.create(null), ids = new Set(this.order);
   for (const provider of this.providers) {
    const token = this.authVersions.get(provider.id);
    const changed = token !== versions.get(provider.id) || pending.has(provider.id) || token?.pending;
    status[provider.id] = (changed ? this.status[provider.id] : provider.status) || { connected: false };
    if (!changed) this.messages.delete(provider.id);
   }
   for (const id of this.messages.keys()) if (!ids.has(id)) this.messages.delete(id);
   for (const id of this.authVersions.keys()) if (!ids.has(id)) {
    this.authVersions.delete(id);
    clearTimeout(this.timer[id]);
    delete this.timer[id];
    delete this.typed[id];
   }
   this.status = status;
   this.build();
  }
  await this.refresh().catch(() => {});
  if (check === this.providerCheck) this.paint();
 }

 // Reads the backend's models again once the list is no longer fresh, quietly: a model added since shows up the next
 // time the picker opens, with no restart. A list that can't be read now leaves the last one in place.
 freshen() {
  if (Date.now() - this.read < FRESH || !Backend.available) return;
  this.read = Date.now();
  this.refresh().catch(() => {});
 }

 // Loads the backend's models into the catalog; the last request wins.
 async refresh() {
  const token = (this.checks = (this.checks || 0) + 1);
  const models = await Backend.request('models.list', {});
  if (token !== this.checks || !Array.isArray(models)) return false;
  this.models = models.filter(model => model?.id && model.provider).map(modelOf);
  this.saveCatalog();
  this.changed();
  return true;
 }

 build() {
  const backend = Backend.available;
  this.list.innerHTML = this.providers.map((provider, index) => section(index, provider.name, provider.methods.map(method =>
   method.type === 'oauth' ? accountRow(method) : keyRow(index, method)).join(''))).join('')
   || `<p class="settings-status" role="status" data-tone="${backend ? '' : 'error'}">${escapeHtml(I18n.t(backend ? 'settings.backend.empty' : 'settings.backend.none'))}</p>`;
  // IDs are opaque strings, not DOM IDs/selectors (even whitespace and NUL must round-trip).
  [...this.list.querySelectorAll('.provider')].forEach((node, index) => {
   node.dataset.provider = this.providers[index].id;
   for (const child of node.querySelectorAll('.settings-key, .settings-status, .settings-account')) child.dataset.provider = node.dataset.provider;
  });
  this.inputs = Object.create(null);
  for (const input of this.list.querySelectorAll('.settings-key')) {
   const provider = input.dataset.provider;
   this.inputs[provider] = input;
   input.value = this.typed[provider] || '';
   input.addEventListener('input', () => this.onKeyInput(provider));
   input.nextElementSibling.addEventListener('click', () => this.reveal(provider, input.type === 'password'));
  }
  this.statuses = Object.create(null);
  for (const node of this.list.querySelectorAll('.settings-status[data-provider]')) (this.statuses[node.dataset.provider] ||= []).push(node);
  this.accounts = Object.assign(Object.create(null), Object.fromEntries([...this.list.querySelectorAll('.settings-account')].map(node => [node.dataset.provider, node])));
  for (const [provider, box] of Object.entries(this.accounts)) {
   box.addEventListener('click', event => {
    const action = event.target.closest('[data-action]')?.dataset.action;
    if (action === 'login') this.login(provider);
    else if (action === 'cancel') this.auth('auth.cancel', provider);
    else if (action === 'logout') this.auth('auth.logout', provider);
   });
  }
  for (const provider of this.order) {
   const message = this.messages.get(provider), error = this.status[provider]?.error;
   this.setStatus(provider, message?.text ?? error ?? '', message?.tone ?? (error ? 'error' : ''));
  }
  this.paint();
 }

 paint() {
  if (!this.list) return;
  for (const node of this.list.querySelectorAll('.provider')) {
   const id = node.dataset.provider, on = this.working(id);
   const count = this.models.filter(model => model.provider === id).length;
   const state = node.querySelector('.provider-state');
   state.textContent = I18n.t(on ? 'settings.connected' : 'settings.off');
   state.classList.toggle('is-on', on);
   node.querySelector('.provider-models').textContent = on && count ? I18n.t('settings.models', { count }) : '';
  }
  for (const [provider, box] of Object.entries(this.accounts || {})) {
   const status = this.status[provider] || {}, account = status.account || {};
   box.dataset.state = status.waiting ? 'waiting' : status.connected ? 'connected' : 'idle';
   const who = [account.email, account.plan && I18n.t('settings.account.plan', { plan: account.plan.charAt(0).toUpperCase() + account.plan.slice(1) })].filter(Boolean).join(' · ');
   box.querySelector('.settings-account-who').textContent = status.waiting ? I18n.t('settings.account.waiting') : who;
  }
  for (const [provider, input] of Object.entries(this.inputs || {})) {
   if (!input.value) input.placeholder = this.status[provider]?.keySaved ? I18n.t('settings.key.saved') : (this.providers.find(item => item.id === provider)?.methods || []).find(method => method.type === 'apiKey')?.placeholder || 'sk-…';
  }
 }

 // How a provider stands, as the backend says: connected or not, waiting for a sign-in, the account, an error.
 setProviderStatus(provider, status) {
  const was = this.connected(provider);
  this.status[provider] = status;
  this.setStatus(provider, status.error || '', status.error ? 'error' : '');
  if (was !== this.connected(provider)) this.refresh().catch(() => {});
  this.paint();
 }

 beginAuth(provider) {
  clearTimeout(this.timer[provider]);
  const token = { pending: true, changed: false };
  this.authVersions.set(provider, token);
  return token;
 }

 async auth(method, provider, extra = {}, token = this.beginAuth(provider)) {
  if (this.authVersions.get(provider) !== token) return null;
  try {
   const result = await Backend.request(method, { provider, ...extra });
   if (this.authVersions.get(provider) !== token) return null;
   const status = statusOf(result);
   if (!status) throw new Error('Invalid provider status from backend.');
   if (!token.changed) this.setProviderStatus(provider, status);
   return status;
  } catch (error) {
   if (this.authVersions.get(provider) !== token || token.changed) return null;
   this.status[provider] = { ...this.status[provider], waiting: false };
   this.setStatus(provider, Backend.explain(error).message, 'error');
   this.paint();
   return null;
  } finally {
   if (this.authVersions.get(provider) === token) {
    token.pending = false;
    // An event during the mutation may describe an older operation. Read again after this one settles.
    if (token.changed) {
     this.status[provider] = { ...this.status[provider], waiting: false };
     this.paint();
     this.refreshAll();
    }
   }
  }
 }

 async login(provider) {
  if (this.status[provider]?.waiting) return;
  this.setStatus(provider, '');
  this.status[provider] = { ...this.status[provider], waiting: true };
  this.paint();
  return this.auth('auth.login', provider);
 }

 open(reason = '', provider = '') {
  const opening = !this.dialog.open;
  if (opening) {
   this.dialog.showModal();
   this.dialog.focus();
   if (Backend.available) this.refreshAll();
  }
  // A missing key opens straight on Providers; otherwise the settings always open on General.
  if (reason) this.page('providers', opening);
  else if (opening) this.page('general', true);
  if (!reason) return;
  const target = provider || this.providers[0]?.id || '';
  this.setStatus(target, reason, 'error');
  const field = this.accounts?.[target]?.querySelector('[data-action="login"]') || this.inputs?.[target];
  field?.scrollIntoView({ block: 'center' });
  field?.focus();
 }

 // A key goes to the backend once the typing stops; the line under the field is for the check and for what went wrong.
 // An emptied field takes the key away.
 onKeyInput(provider) {
  const key = this.inputs[provider].value.trim();
  this.typed[provider] = key;
  const token = this.beginAuth(provider);
  this.setStatus(provider, key ? I18n.t('settings.key.checking') : '');
  this.timer[provider] = setTimeout(() => this.saveKey(provider, key, token), CHECK_DELAY);
 }

 async saveKey(provider, key, token = this.beginAuth(provider)) {
  const status = await this.auth('auth.setKey', provider, { key: key || null }, token);
  if (this.authVersions.get(provider) !== token || !status) return;
  if (!status.error && key && key === this.typed[provider]) {
   this.typed[provider] = '';
   if (this.inputs[provider]) this.inputs[provider].value = '';
   this.paint();
  }
  await this.refresh().catch(() => {});
 }

 setStatus(provider, text, tone = '') {
  this.messages.set(provider, { text, tone });
  for (const node of this.statuses?.[provider] || []) {
   node.textContent = text;
   node.dataset.tone = tone;
  }
 }
}

window.Settings = Settings;
})();
