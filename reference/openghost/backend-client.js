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
const { validate, isObject, isId, has } = window.BackendProtocol;

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

// Preserve named ABP errors; otherwise normalize JSON-RPC's standard errors in both directions.
const rpcCode = code => code === -32601 ? 'unsupported' : [-32700, -32600, -32602].includes(code) ? 'invalid_request' : 'unknown';
function fromRpc(error) {
 const data = isObject(error.data) ? error.data : {};
 return new BackendError({ ...data,
  code: typeof data.code === 'string' && data.code ? data.code : rpcCode(error.code),
  message: typeof data.message === 'string' && data.message ? data.message : error.message,
 });
}

// Process recovery is manual: the desktop host never restarts a stopped backend.
function processMessage(status) {
 if (status?.state === 'error') return `Backend error: ${status.error || 'Unable to connect.'} Check OPENGHOST_BACKEND or backend.json, then relaunch OpenGhost.`;
 if (status?.state === 'exited') {
  const details = [status.code != null && `exit code ${status.code}`, status.signal && `signal ${status.signal}`].filter(Boolean);
  return `The backend stopped${details.length ? ` (${details.join(', ')})` : ''}. Relaunch OpenGhost to reconnect.`;
 }
 if (status?.state === 'running') return 'The backend is still connecting.';
 if (status?.state === 'stopped') return 'The backend is not running. Relaunch OpenGhost to reconnect.';
 if (status?.state === 'disposed') return 'This backend client has been disposed.';
 return 'No backend is configured. Set OPENGHOST_BACKEND or backend.json, then relaunch OpenGhost.';
}

class BackendClient {
 // `transport`: { send(message), onMessage(callback), onStatus(callback), status(): Promise<{ state }> }, or null.
 // Both subscriptions return unsubscribe functions. Dispose this client before replacing it.
 // `hello()`: what the client tells the backend at the handshake (client, host tools, render guide), asked for then.
 constructor(transport, { hello = () => ({}), timeouts = {} } = {}) {
  this.transport = transport;
  this.hello = hello;
  this.timeouts = { ...TIMEOUTS, ...timeouts };
  this.connectionId = globalThis.crypto?.randomUUID?.() || `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  this.next = 1;
  this.pending = new Map();
  this.listeners = new Map();
  this.handlers = new Map();
  this.incoming = new Map();
  this.state = transport ? 'connecting' : 'unavailable';
  this.info = null;
  this.capabilities = {};
  this.process = { state: 'none' };
  this.failure = null;
  this.disposed = false;
  // Each backend connection (a `running` status) is one epoch; a handshake continuation from an older one is dropped.
  this.epoch = 0;
  this.subscriptions = [];
  this.ready = new Promise(resolve => { this.settleReady = resolve; });
  if (!transport) { this.settleReady(false); return; }
  this.subscriptions.push(transport.onMessage(message => this.receive(message)), transport.onStatus(status => this.onStatus(status)));
  Promise.resolve(transport.status()).then(status => this.onStatus(status), error => this.onStatus({ state: 'error', error: error?.message || 'Unable to read backend status.' }));
 }

 get available() {
  return this.state === 'ready';
 }

 // Whether the backend said it can do something, by a path into its capabilities: can('turns.cancel').
 can(path) {
  return !!path.split('.').reduce((value, key) => value?.[key], this.capabilities);
 }

 onStatus(status) {
  if (this.disposed || !status) return;
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
  if (this.disposed) return;
  const epoch = ++this.epoch, stale = () => this.disposed || epoch !== this.epoch;
  this.state = 'initializing';
  this.failure = null;
  try {
   await loaded;
   if (stale()) return;
   const result = await this.call('initialize', { ...this.hello(), protocolVersion: PROTOCOL, connectionId: this.connectionId });
   if (stale()) return;
   if (!isObject(result) || typeof result.protocolVersion !== 'string') throw new BackendError({ code: 'protocol_error', message: 'Invalid initialize result: expected a string protocolVersion.' });
   if (result.protocolVersion !== PROTOCOL) throw new BackendError({ code: 'unsupported', message: `Unsupported backend protocolVersion: ${result.protocolVersion} (expected ${PROTOCOL}).` });
   this.info = result?.backend || null;
   this.capabilities = result?.capabilities || {};
   this.state = 'ready';
   this.emit('ready', this.info);
   this.settleReady(true);
  } catch (error) {
   if (stale()) return;
   this.state = 'unavailable';
   this.info = null;
   this.capabilities = {};
   this.failure ||= new BackendError({ code: 'backend_unavailable', message: `Backend initialization failed: ${error.message} Check the backend and relaunch OpenGhost.` });
   this.emit('closed', { state: 'error', error: error.message });
   this.settleReady(false);
  }
 }

 // The backend went away: whatever was waiting for it fails, and whatever it was waiting for is let go.
 close(status) {
  const was = this.state;
  this.epoch++;
  this.state = 'unavailable';
  this.info = null;
  this.capabilities = {};
  const error = this.failure = new BackendError({
   code: was === 'ready' ? 'backend_crashed' : 'backend_unavailable', message: processMessage(status),
  });
  for (const { reject } of this.pending.values()) reject(error);
  this.pending.clear();
  for (const controller of this.incoming.values()) controller.abort();
  this.incoming.clear();
  if (was === 'ready' || was === 'initializing' || was === 'connecting') this.emit('closed', status);
  this.settleReady(false);
 }

 // Release bridge subscriptions and outstanding work, without stopping the backend process.
 dispose() {
  if (this.disposed) return;
  this.disposed = true;
  for (const unsubscribe of this.subscriptions.splice(0)) if (typeof unsubscribe === 'function') unsubscribe();
  this.close({ state: 'disposed' });
  this.listeners.clear();
  this.handlers.clear();
  this.transport = null;
 }

 unavailable() {
  return new BackendError({ code: 'backend_unavailable', message: this.failure?.message || (!this.transport
   ? 'No backend bridge is available. Open the OpenGhost desktop app with a configured backend.'
   : processMessage(this.process)) });
 }

 // A request to the backend. Resolves with its result, rejects with a BackendError (`timeout` when it goes unanswered
 // for its method's time), or with an AbortError once `signal` aborts. On a timeout or an abort the backend is told to
 // stop working on it, and a late answer is ignored.
 request(method, params, { signal } = {}) {
  if (this.state !== 'ready') return Promise.reject(this.unavailable());
  return this.call(method, params, { signal });
 }

 call(method, params, { signal } = {}) {
  if (this.disposed) return Promise.reject(this.unavailable());
  if (signal?.aborted) return Promise.reject(aborted());
  const id = `${this.connectionId}:${this.next++}`;
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
   // A request the host could not hand to the backend (stdin closed or broken, queue full, invalid) fails at once.
   const undelivered = () => {
    if (!this.pending.delete(id)) return;
    settle(reject)(new BackendError({ code: 'backend_unavailable', message: `The backend could not be sent ${method}: its input is closed or full.` }));
   };
   const sent = this.send({ jsonrpc: '2.0', id, method, ...(params === undefined ? {} : { params }) });
   if (sent === false) undelivered();
   else if (typeof sent?.then === 'function') sent.then(ok => { if (ok === false) undelivered(); }, undelivered);
  });
 }

 notify(method, params) {
  if (!this.transport) return;
  this.send({ jsonrpc: '2.0', method, ...(params === undefined ? {} : { params }) });
 }

 // The transport's answer: `false` (or a promise of it) when the message was not delivered.
 send(message) {
  return this.disposed ? false : this.transport.send(message);
 }

 // Events from the backend by method ('message.delta', 'usage', …), and the client's own 'ready' and 'closed';
 // '*' hears every event as (method, params). Returns a function that stops listening.
 on(method, listener) {
  if (this.disposed) return () => {};
  if (!this.listeners.has(method)) this.listeners.set(method, new Set());
  this.listeners.get(method).add(listener);
  return () => this.listeners.get(method)?.delete(listener);
 }

 emit(method, params) {
  const call = (listener, ...args) => {
   try { listener(...args); } catch (error) { console.error('Backend event listener failed', method, error); }
  };
  for (const listener of this.listeners.get(method) || []) call(listener, params, method);
  if (method !== 'ready' && method !== 'closed') for (const listener of this.listeners.get('*') || []) call(listener, method, params);
 }

 // What answers the backend's own requests (approval.request, host.tool): `handler(params, { signal })` returns the
 // result or throws. The signal aborts when the backend sends $/cancelRequest for it.
 handle(method, handler) {
  if (!this.disposed) this.handlers.set(method, handler);
 }

 receive(message) {
  if (this.disposed) return;
  const invalid = validate(message);
  if (invalid) {
   if (!isObject(message)) return;
   // A response (including a mixed request/response) must never resolve a call or execute a reverse handler.
   const response = !has(message, 'method') || has(message, 'result') || has(message, 'error');
   const waiting = has(message, 'id') && isId(message.id) && response && this.pending.get(message.id);
   if (waiting) {
    this.pending.delete(message.id);
    waiting.reject(new BackendError({ code: 'protocol_error', message: invalid.message }));
   } else if (has(message, 'method') && has(message, 'id')) {
    this.replyError(isId(message.id) ? message.id : null, invalid.code, invalid.message);
   }
   // Uncorrelated response-only envelopes and malformed notifications are ignored.
   return;
  }
  if (!has(message, 'method')) {
   const waiting = this.pending.get(message.id);
   if (!waiting) return;
   this.pending.delete(message.id);
   if (has(message, 'error')) waiting.reject(fromRpc(message.error));
   else waiting.resolve(message.result);
   return;
  }
  if (!has(message, 'id')) {
   if (message.method === '$/cancelRequest') {
    if (isId(message.params?.id)) this.incoming.get(message.params.id)?.abort();
   } else if (this.available) this.emit(message.method, message.params || {});
   return;
  }
  if (!this.available) { this.replyError(message.id, -32000, this.unavailable().message, 'backend_unavailable'); return; }
  this.answer(message);
 }

 replyError(id, code, message, appCode = rpcCode(code)) {
  this.send({ jsonrpc: '2.0', id, error: { code, message, data: { code: appCode, message } } });
 }

 async answer({ id, method, params }) {
  const handler = this.handlers.get(method);
  if (!handler) {
   this.replyError(id, -32601, `Method not found: ${method}`);
   return;
  }
  // An id still being answered is not taken again: its handler would run twice, and one answer would take the other's place.
  if (this.incoming.has(id)) {
   const message = `Request ${id} is already being answered`;
   this.replyError(id, -32600, message, 'duplicate_request');
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
   this.replyError(id, code === 'unsupported' ? -32601 : code === 'invalid_request' ? -32602 : -32000, error?.message || code, code);
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
window.Backend?.dispose();
window.Backend = new BackendClient(window.openghost?.backend || null, { hello });
window.Backend.explain = explain;
})();
