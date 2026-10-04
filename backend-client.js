(() => {
'use strict';

// The UI's only way to an agent. Everything the chat shows, from the streamed reply to the approval cards, the usage,
// the models and the sign-ins, comes from an external backend through this client, as the Agent Backend Protocol
// (ABP v0, docs/backend-interface.md) describes: JSON-RPC 2.0, where both sides may ask and the backend streams events.
//
//   OpenGhost UI  →  window.Backend (this file)  →  preload `backend` bridge  →  desktop/backend-host.js  →  backend process
//
// There is no model, prompt, key or tool code in the app. With no backend connected every request fails with a
// `backend_unavailable` error, and the chat says so.
const PROTOCOL = '0.1';

// The handshake waits for the page's scripts, so it can name the host tools and the render guide they define.
const loaded = typeof document === 'undefined' || document.readyState !== 'loading'
 ? Promise.resolve()
 : new Promise(resolve => document.addEventListener('DOMContentLoaded', resolve, { once: true }));

class BackendError extends Error {
 constructor({ code = 'unknown', message = '', action, status, provider, retryable } = {}) {
  super(message);
  this.name = 'BackendError';
  this.code = code;
  this.action = action;
  this.status = status;
  this.provider = provider;
  this.retryable = retryable;
 }
}

const aborted = () => new DOMException('Aborted', 'AbortError');

// How long a request may go unanswered before it fails with `timeout` and the backend is told to stop working on it, so
// a hung backend can't leave anything waiting forever. Sign-ins wait for the user in a browser, and a compaction runs a
// model, so theirs are long; everything else is a quick answer.
const TIMEOUTS = { default: 60e3, 'auth.login': 15 * 60e3, 'session.compact': 10 * 60e3 };

// A JSON-RPC error as the backend sent it: its ABP error in `data` when there is one.
function fromRpc(error) {
 const data = error?.data && typeof error.data === 'object' ? error.data : {};
 return new BackendError({ ...data, code: data.code || (error?.code === -32601 ? 'unsupported' : 'unknown'), message: data.message || error?.message || '' });
}

class BackendClient {
 // `transport`: { send(message), onMessage(callback), onStatus(callback), status(): Promise<{ state }> }, or null.
 // `hello()`: what the client tells the backend at the handshake (client, host tools, render guide), asked for then.
 constructor(transport, { hello = () => ({}), timeouts = {} } = {}) {
  this.transport = transport;
  this.hello = hello;
  this.timeouts = { ...TIMEOUTS, ...timeouts };
  this.next = 1;
  this.pending = new Map();
  this.listeners = new Map();
  this.handlers = new Map();
  this.incoming = new Map();
  this.state = transport ? 'connecting' : 'unavailable';
  this.info = null;
  this.capabilities = {};
  this.process = { state: 'none' };
  this.ready = new Promise(resolve => { this.settleReady = resolve; });
  if (!transport) { this.settleReady(false); return; }
  transport.onMessage(message => this.receive(message));
  transport.onStatus(status => this.onStatus(status));
  Promise.resolve(transport.status()).then(status => this.onStatus(status), () => this.onStatus({ state: 'none' }));
 }

 get available() {
  return this.state === 'ready';
 }

 // Whether the backend said it can do something, by a path into its capabilities: can('turns.cancel').
 can(path) {
  return !!path.split('.').reduce((value, key) => value?.[key], this.capabilities);
 }

 onStatus(status) {
  if (!status) return;
  const same = status.state === this.process.state;
  this.process = status;
  if (status.state === 'running') {
   if (this.state !== 'ready' && this.state !== 'initializing') this.initialize();
   return;
  }
  // The first word may be that there is no backend: the client is then settled as unavailable.
  if (!same || this.state === 'connecting') this.close(status);
 }

 async initialize() {
  this.state = 'initializing';
  try {
   await loaded;
   const result = await this.call('initialize', { protocolVersion: PROTOCOL, ...this.hello() });
   this.info = result?.backend || null;
   this.capabilities = result?.capabilities || {};
   this.state = 'ready';
   this.emit('ready', this.info);
   this.settleReady(true);
  } catch (error) {
   this.state = 'unavailable';
   this.info = null;
   this.capabilities = {};
   this.emit('closed', { state: 'error', error: error.message });
   this.settleReady(false);
  }
 }

 // The backend went away: whatever was waiting for it fails, and whatever it was waiting for is let go.
 close(status) {
  const was = this.state;
  this.state = 'unavailable';
  this.info = null;
  this.capabilities = {};
  const error = new BackendError(was === 'ready'
   ? { code: 'backend_crashed', message: `The backend stopped${status?.code != null ? ` (exit code ${status.code})` : ''}.` }
   : { code: 'backend_unavailable', message: 'No backend is connected.' });
  for (const { reject } of this.pending.values()) reject(error);
  this.pending.clear();
  for (const controller of this.incoming.values()) controller.abort();
  this.incoming.clear();
  if (was === 'ready' || was === 'initializing') this.emit('closed', status);
  this.settleReady(false);
 }

 unavailable() {
  return new BackendError({ code: 'backend_unavailable', message: 'No backend is connected.' });
 }

 // A request to the backend. Resolves with its result, rejects with a BackendError (`timeout` when it goes unanswered
 // for its method's time), or with an AbortError once `signal` aborts. On a timeout or an abort the backend is told to
 // stop working on it, and a late answer is ignored.
 request(method, params, { signal } = {}) {
  if (this.state !== 'ready') return Promise.reject(this.unavailable());
  return this.call(method, params, { signal });
 }

 call(method, params, { signal } = {}) {
  if (signal?.aborted) return Promise.reject(aborted());
  const id = this.next++;
  return new Promise((resolve, reject) => {
   let timer;
   const settle = done => value => { clearTimeout(timer); signal?.removeEventListener('abort', stop); done(value); };
   const give = error => {
    if (!this.pending.delete(id)) return;
    this.notify('$/cancelRequest', { id });
    settle(reject)(error);
   };
   const stop = () => give(aborted());
   this.pending.set(id, { resolve: settle(resolve), reject: settle(reject) });
   signal?.addEventListener('abort', stop, { once: true });
   const ms = this.timeouts[method] ?? this.timeouts.default;
   timer = setTimeout(() => give(new BackendError({ code: 'timeout', message: `The backend did not answer ${method} in time.` })), ms);
   this.send({ jsonrpc: '2.0', id, method, ...(params === undefined ? {} : { params }) });
  });
 }

 notify(method, params) {
  if (!this.transport) return;
  this.send({ jsonrpc: '2.0', method, ...(params === undefined ? {} : { params }) });
 }

 send(message) {
  this.transport.send(message);
 }

 // Events from the backend by method ('message.delta', 'usage', …), and the client's own 'ready' and 'closed';
 // '*' hears every event as (method, params). Returns a function that stops listening.
 on(method, listener) {
  if (!this.listeners.has(method)) this.listeners.set(method, new Set());
  this.listeners.get(method).add(listener);
  return () => this.listeners.get(method)?.delete(listener);
 }

 emit(method, params) {
  for (const listener of this.listeners.get(method) || []) listener(params, method);
  if (method !== 'ready' && method !== 'closed') for (const listener of this.listeners.get('*') || []) listener(method, params);
 }

 // What answers the backend's own requests (approval.request, host.tool): `handler(params, { signal })` returns the
 // result or throws. The signal aborts when the backend sends $/cancelRequest for it.
 handle(method, handler) {
  this.handlers.set(method, handler);
 }

 receive(message) {
  if (!message || message.jsonrpc !== '2.0') return;
  if (message.method === undefined) {
   const waiting = this.pending.get(message.id);
   if (!waiting) return;
   this.pending.delete(message.id);
   if (message.error) waiting.reject(fromRpc(message.error));
   else waiting.resolve(message.result);
   return;
  }
  if (message.id === undefined || message.id === null) {
   if (message.method === '$/cancelRequest') this.incoming.get(message.params?.id)?.abort();
   else this.emit(message.method, message.params || {});
   return;
  }
  this.answer(message);
 }

 async answer({ id, method, params }) {
  const handler = this.handlers.get(method);
  if (!handler) {
   this.send({ jsonrpc: '2.0', id, error: { code: -32601, message: `Method not found: ${method}` } });
   return;
  }
  // An id still being answered is not taken again: its handler would run twice, and one answer would take the other's place.
  if (this.incoming.has(id)) {
   const message = `Request ${id} is already being answered`;
   this.send({ jsonrpc: '2.0', id, error: { code: -32600, message, data: { code: 'duplicate_request', message } } });
   return;
  }
  const controller = new AbortController();
  this.incoming.set(id, controller);
  try {
   const result = await handler(params || {}, { signal: controller.signal });
   if (this.incoming.get(id) === controller) this.send({ jsonrpc: '2.0', id, result: result ?? null });
  } catch (error) {
   if (this.incoming.get(id) !== controller) return;
   const code = error instanceof BackendError ? error.code : error?.name === 'AbortError' ? 'cancelled' : 'unknown';
   this.send({ jsonrpc: '2.0', id, error: { code: -32000, message: error?.message || code, data: { code, message: error?.message || code } } });
  } finally {
   if (this.incoming.get(id) === controller) this.incoming.delete(id);
  }
 }
}

// An error as the chat shows it: the backend's own words, or the app's for the errors it knows by their code, and
// whether a button to the settings belongs under it.
function explain(error) {
 const t = (key, values) => window.I18n?.has(key) ? I18n.t(key, values) : '';
 const provider = error?.provider || '';
 const known = {
  backend_unavailable: () => t('error.backend.none'),
  backend_crashed: () => t('error.backend.crashed'),
  auth: () => t('error.auth', { provider: provider || 'the provider' }),
  quota: () => t('error.quota', { provider: provider || 'The provider' }),
  rate_limit: () => t('error.rate', { provider: provider || 'The provider' }),
  server: () => t('error.server', { provider: provider || 'The provider' }),
  network: () => t('error.connect', { provider: provider || 'the provider' }),
  context_overflow: () => t('error.context'),
 }[error?.code];
 return { message: error?.message || known?.() || t('error.unknown') || 'Something went wrong.', settings: error?.action === 'open-settings' || error?.code === 'auth' };
}

// What the client tells the backend about itself at the handshake: the app, where it runs, the tools the host runs for
// the backend (the built-in browser), and the guide to what this renderer can draw.
function hello() {
 const bridge = window.openghost;
 return {
  client: { name: 'OpenGhost', version: '1.3.0', platform: bridge?.platform || 'web', locale: navigator.language || 'en' },
  host: {
   tools: window.HostTools?.schemas || [],
   renderGuide: window.RenderGuide || '',
   attachments: { localPaths: !!bridge?.desktop },
  },
 };
}

window.BackendError = BackendError;
window.BackendClient = BackendClient;
window.Backend = new BackendClient(window.openghost?.backend || null, { hello });
window.Backend.explain = explain;
})();
