(() => {
'use strict';

const FOLLOW_DISTANCE = 48;
const FOLLOW_SPRING = [130, 23];
const BOTTOM_SHOW = 120;
const JUMP = { base: 420, perPixel: 0.05, max: 950 };
const COPIED_TIME = 1600;
const FINISH_NOTES = ['length', 'content_filter', 'insufficient_system_resource'];
const LEAVE = { duration: 260, easing: 'cubic-bezier(0.32, 0.72, 0, 1)', fill: 'forwards' };
const SWITCH = { duration: 280, easing: 'cubic-bezier(0.32, 0.72, 0, 1)' };
const PIN_TIME = 2000;
// How far below the top of the chat a card too tall for the room over the composer keeps its top: where the first message sits.
const ANCHOR_GAP = 56;
// The chat on screen keeps its messages under the lock screen while it frosts over (FROST in lock-ui.js), then lets them go.
const LOCK_FADE = 700;
const REMOVE = { duration: 240, easing: 'cubic-bezier(0.32, 0.72, 0, 1)', fill: 'forwards' };
// An approval card the user answered by sending a message instead.
const SUPERSEDED = 'superseded';

const reducedMotion = () => window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const samePath = (a, b) => Library.samePath(a, b);
const uid = () => `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;

// One attachment as the backend gets it (ABP Attachment), once the app has read it: the picture of an image, the text of
// a text file, a video's size and length, and the file's place on the disk when it has one.
function attachmentOf(item, payload, n) {
 const kind = payload.type === 'image' ? 'image' : payload.type === 'video' ? 'video' : payload.pdf ? 'pdf' : payload.type === 'text' ? 'text' : 'file';
 const out = { id: `a${n}`, name: item.name, mime: item.file?.type || '', size: item.size || 0, kind };
 if (item.note) out.note = item.note;
 if (payload.path) out.path = payload.path;
 if (kind === 'image') Object.assign(out, { dataUrl: payload.url, width: payload.width, height: payload.height });
 if (payload.type === 'text') Object.assign(out, { text: payload.text, truncated: !!payload.truncated });
 if (kind === 'video') out.video = { duration: payload.duration || 0, width: payload.width || 0, height: payload.height || 0 };
 return out;
}

// A message as the backend gets it (ABP Input): its text and its attachments, once every attachment is read.
async function inputOf({ text, attachments }) {
 const payloads = await Promise.all(attachments.map(item => item.ready));
 return { text, attachments: attachments.map((item, k) => attachmentOf(item, payloads[k] || {}, k + 1)) };
}

// Several messages sent while nothing could take them, as one.
function combine(inputs) {
 let n = 0;
 return {
  text: inputs.map(input => input.text).filter(Boolean).join('\n\n'),
  attachments: inputs.flatMap(input => input.attachments).map(item => ({ ...item, id: `a${++n}` })),
 };
}

// A pasted text keeps only its first line and length here; the text itself went to the backend with the message.
// A picture keeps itself, to be shown again when the chat opens; a video its place on the disk and the frame its card shows.
const slim = ({ name, size, image, width, height, note, pasted, payload }) => ({
 name, size, image: !!image, width, height, note,
 url: image && payload?.type === 'image' ? payload.url : undefined,
 pasted: pasted && { preview: pasted.preview, lines: pasted.lines },
 video: payload?.type === 'video' ? { path: payload.path, duration: payload.duration, poster: payload.poster } : undefined,
});
const join = (base, text) => [base.trimEnd(), text.trim()].filter(Boolean).join('\n\n');
function splitQuotes(text) {
 const quotes = [];
 let rest = text || '', m;
 while ((m = rest.match(/^\s*((?:>[^\n]*(?:\n|$))+)/))) {
  quotes.push(m[1].replace(/^> ?/gm, '').trim());
  rest = rest.slice(m[0].length);
 }
 return { quotes: quotes.filter(Boolean), rest: rest.trim() };
}

// Every chat window (the main one and each mini chat), so the backend's events reach the one they are for.
const chats = new Set();

const drop = (list, item) => {
 const at = list.indexOf(item);
 if (at >= 0) list.splice(at, 1);
};

// Tokens as a chat counts them: sent, of them read from the provider's cache or written to it, written back, and requests.
const tokens = () => ({ input: 0, cached: 0, written: 0, output: 0, requests: 0 });
const addUp = (into, usage) => {
 for (const key of Object.keys(into)) into[key] += usage[key] || 0;
 return into;
};

// A reply or a summary keeps what its requests cost, as the backend's usage events count it, so the chat can later tell
// what it spent and on which model.
function spend(entry, usage) {
 const parts = Usage.parts(usage);
 if (parts) addUp(entry.usage ||= tokens(), parts);
}

const aborted = () => new DOMException('Aborted', 'AbortError');

function settle(root) {
 for (const animation of root.getAnimations({ subtree: true })) {
  if (animation.effect?.getComputedTiming().iterations === Infinity) continue;
  try { animation.finish(); } catch {}
 }
}

function collapse(el) {
 if (!el.isConnected) return;
 if (reducedMotion()) { el.remove(); return; }
 el.style.overflow = 'hidden';
 el.animate([{ height: `${el.offsetHeight}px`, opacity: 1 }, { height: '0px', marginTop: '0px', paddingTop: '0px', opacity: 0 }], REMOVE)
  .finished.then(() => el.remove(), () => el.remove());
}

class Conversation {
 constructor(record, list = document.createElement('div')) {
  this.record = record;
  this.folder = null;
  this.model = '';
  this.messages = [];
  // How full the backend says the chat's context is: tokens used, out of the model's window.
  this.tokens = 0;
  this.window = 0;
  this.list = list;
  this.list.className = 'thread-list';
  this.list.__conversation = this;
  this.turn = null;
  this.follow = true;
  this.scrollTop = 0;
  this.unread = false;
  this.ready = null;
 }

 get id() {
  return this.record?.id || '';
 }
}

class Chat {
 constructor({ main, thread, bottom, settings, library, onChange, onList }) {
  this.main = main;
  this.thread = thread;
  this.bottom = bottom;
  this.settings = settings;
  this.library = library;
  this.onChange = onChange;
  this.onList = onList;
  this.conversations = new Map();
  this.nodes = new WeakMap();
  this.active = null;
  this.draft = null;
  this.opening = 0;
  this.tools = 0;
  this.follow = true;
  this.lastTop = 0;
  this.followFrame = 0;
  this.followLast = 0;
  this.followPos = 0;
  this.followVel = 0;
  this.followMoving = false;
  this.jumpFrame = 0;
  this.followStep = this.followStep.bind(this);
  this.pinUntil = 0;
  this.resize = new ResizeObserver(() => {
   if (this.follow && this.busy) this.followBottom();
   else if (this.follow && performance.now() < this.pinUntil) this.pin();
   this.syncBottom();
  });
  thread.addEventListener('scroll', () => this.onScroll());
  thread.addEventListener('click', event => this.onClick(event));
  thread.addEventListener('diagram-edit', event => this.onDiagramEdit(event));
  thread.addEventListener('stats-remove', event => this.onStatsRemove(event));
  bottom.addEventListener('scroll-bottom', () => this.scrollToBottom());
  new RowGlide(thread);
  this.activate(this.newDraft(thread.querySelector('.thread-list') || undefined));
  chats.add(this);
 }

 // The backend's name for a chat's session: the chat's own id.
 sessionOf(conv) {
  return conv?.id || '';
 }

 bySession(sessionId) {
  if (!sessionId) return null;
  for (const conv of this.conversations.values()) if (this.sessionOf(conv) === sessionId) return conv;
  return null;
 }

 // This chat no longer hears the backend: its window is gone.
 dispose() {
  chats.delete(this);
 }

 get busy() {
  return !!this.active?.turn;
 }

 get model() {
  return this.modelOf(this.active);
 }

 modelOf(conv) {
  return this.settings.resolve(conv?.record?.model || conv?.model);
 }

 config(conv) {
  return this.settings.configFor(this.modelOf(conv));
 }

 // Whether the chat has turns no summary covers yet: a new model would need them compacted first.
 hasHistory(conv = this.active) {
  const messages = conv?.messages || [], last = messages.findLastIndex(entry => entry.role === 'compact');
  return messages.slice(last + 1).some(entry => entry.role === 'user' || entry.role === 'assistant');
 }

 setModel(id) {
  const conv = this.active;
  if (!conv || conv.turn || id === this.modelOf(conv)) return;
  if (conv.record) this.library.update(conv.id, { model: id });
  else conv.model = id;
  this.settings.setModel(id);
  this.onChange();
 }

 // The backend may compact the chat with the model it worked with first, so the new one starts from a history that fits
 // its own window; the chat shows that as it would its own compaction.
 switchModel(id) {
  const conv = this.active;
  if (!conv?.record || conv.turn || id === this.modelOf(conv)) return;
  if (!this.hasHistory(conv) || !Backend.available) { this.setModel(id); return; }
  const turn = this.begin(conv, this.config(conv));
  turn.switch = this.modelOf(conv);
  this.library.update(conv.id, { model: id });
  this.settings.setModel(id);
  this.summarize(conv, turn, async () => {
   const config = this.config(conv);
   await Backend.request('session.configure', { sessionId: this.sessionOf(conv), model: config.model, provider: config.provider, thinking: config.effort }, { signal: turn.controller.signal });
   turn.switch = '';
   turn.config = config;
   return null;
  });
 }

 // Whether the chat on screen can be compacted now: the backend can, the chat has turns no summary covers yet, and
 // nothing is being written.
 get canCompact() {
  const conv = this.active;
  return !!conv?.record && !conv.turn && !conv.locked && Backend.can('compaction.manual') && this.hasHistory(conv);
 }

 // Whether the chat on screen has anything to count: a message in it, and no reply being written.
 get canStats() {
  const conv = this.active;
  return !!conv?.record && !conv.turn && !conv.locked && conv.messages.some(entry => entry.role === 'user' || entry.role === 'assistant');
 }

 // How full the chat's context is, out of its model's window, from 0 to 1, as the backend last reported it.
 get fill() {
  const conv = this.active;
  return conv?.record ? Math.min(1, (conv.tokens || 0) / (conv.window || this.settings.windowOf(this.modelOf(conv)))) : 0;
 }

 // Compacts the chat on screen on the user's word: the backend summarizes it, and the chat shows it as a full window would.
 compactNow() {
  const conv = this.active;
  if (!this.canCompact) return false;
  const turn = this.begin(conv, this.config(conv));
  turn.compacting = true;
  this.summarize(conv, turn, async () => {
   await Backend.request('session.compact', { sessionId: this.sessionOf(conv) }, { signal: turn.controller.signal });
   turn.compacting = false;
   return null;
  });
  return true;
 }

 // A turn with nothing of its own to say, only the summary's line in the chat. Messages sent meanwhile wait and go right after it.
 summarize(conv, turn, work) {
  turn.quiet = true;
  this.openPart(conv, turn);
  const view = turn.part.view;
  view.status.remove();
  view.el.hidden = true;
  this.follow = true;
  this.onChange();
  this.drive(conv, turn, work);
  this.followBottom();
 }

 get activeId() {
  return this.active?.id || '';
 }

 get folder() {
  return this.active && !this.active.record ? this.active.folder : null;
 }

 // A new chat not written in yet: it has no place in the list until its first message, only a stand-in row.
 get isDraft() {
  return !!this.active && !this.active.record;
 }

 isBusy(id) {
  return !!this.conversations.get(id)?.turn;
 }

 isUnread(id) {
  return !!this.conversations.get(id)?.unread;
 }

 setFolder(folder) {
  if (!this.active || this.active.record) return;
  this.active.folder = folder;
  this.onChange();
 }

 newDraft(list) {
  return this.draft = new Conversation(null, list);
 }

 newChat(folder = null) {
  const draft = this.draft || this.newDraft();
  draft.folder = folder;
  draft.model = '';
  this.opening++;
  this.activate(draft);
  this.onChange();
 }

 open(id) {
  if (this.active?.id === id) return Promise.resolve();
  const token = ++this.opening;
  let conv = this.conversations.get(id);
  if (!conv) {
   const record = this.library.chat(id);
   if (!record) return Promise.resolve();
   conv = new Conversation(record);
   this.conversations.set(id, conv);
   // A locked chat opens onto its lock screen; its messages are read only once the password is in.
   if (this.library.isLocked(id)) conv.locked = true;
   else conv.ready = this.load(conv);
  }
  return Promise.resolve(conv.ready).then(() => {
   if (token !== this.opening) return;
   this.activate(conv);
   this.onChange();
  });
 }

 load(conv) {
  return this.library.conversation(conv.id).then(({ messages, tokens }) => {
   conv.messages = messages;
   conv.tokens = tokens;
   this.restore(conv);
  });
 }

 isLocked(id) {
  const conv = this.conversations.get(id);
  return conv ? !!conv.locked : this.library.isLocked(id);
 }

 // Locks a protected chat: its view closes at once, and its messages leave memory as soon as no reply is being written into them.
 seal(conv, delay = 0) {
  conv.locked = true;
  const drop = () => {
   if (!conv.locked || conv.turn) return;
   this.library.relock(conv.id);
   conv.messages = [];
   conv.tokens = 0;
   conv.ready = null;
   conv.list.replaceChildren();
  };
  if (delay && !reducedMotion()) setTimeout(drop, delay);
  else drop();
 }

 lock(id) {
  const conv = this.conversations.get(id);
  if (!conv || conv.locked || !this.library.isProtected(id)) return;
  this.seal(conv, conv === this.active ? LOCK_FADE : 0);
  this.onChange();
 }

 // The password opens the chat; its messages are read again unless a reply still running kept them in memory.
 async unlock(id, password) {
  const conv = this.conversations.get(id);
  if (!conv?.locked) return true;
  if (!(await this.library.unlock(id, password))) return false;
  if (!conv.ready) {
   try {
    await (conv.ready = this.load(conv));
   } catch (error) {
    // Messages that would not open must never pass for an empty chat: a reply saved into it would overwrite them.
    conv.ready = null;
    this.library.relock(id);
    throw error;
   }
  }
  conv.locked = false;
  if (conv === this.active) {
   this.main.classList.toggle('is-empty', !conv.list.childElementCount);
   this.follow = true;
   this.pin();
   this.pinUntil = performance.now() + PIN_TIME;
  }
  this.onChange();
  return true;
 }

 // Setting a password locks the chat straight away, so the first thing its owner does is open it with the new password.
 async protect(id, password) {
  const conv = this.conversations.get(id);
  if (conv?.turn || conv?.locked) return false;
  const loaded = conv?.ready ? { messages: conv.messages, tokens: conv.tokens } : null;
  if (!(await this.library.protect(id, password, loaded))) return false;
  if (conv) this.seal(conv, conv === this.active ? LOCK_FADE : 0);
  this.onChange();
  return true;
 }

 async unprotect(id) {
  const conv = this.conversations.get(id);
  if (!conv || conv.locked) return false;
  const done = await this.library.unprotect(id, conv.ready ? { messages: conv.messages, tokens: conv.tokens } : null);
  this.onChange();
  return done;
 }

 remove(id) {
  const conv = this.conversations.get(id), record = this.library.chat(id);
  if (conv) {
   this.abort(conv);
   this.conversations.delete(id);
  }
  if (record && Backend.can('sessions.delete')) Backend.request('session.delete', { sessionId: conv ? this.sessionOf(conv) : id }).catch(() => {});
  if (this.active?.id === id) {
   const folder = record && !this.library.isHome(record) && this.library.folders.find(item => samePath(item.path, record.folder));
   this.newChat(folder ? { path: folder.path, name: folder.name } : null);
  }
  conv?.list.remove();
 }

 removeFolder(path, ids) {
  for (const id of ids) this.remove(id);
  const gone = folder => folder && samePath(folder.path, path);
  if (this.active && !this.active.record && gone(this.active.folder)) this.newChat(null);
  if (this.draft && gone(this.draft.folder)) this.draft.folder = null;
 }

 attach(conv) {
  if (conv.list.isConnected) return;
  conv.list.classList.add('is-parked');
  this.thread.append(conv.list);
  this.onList?.(conv.list);
 }

 activate(conv) {
  const prev = this.active;
  if (prev === conv) return;
  this.stopFollow();
  this.anchor = null;
  if (prev) {
   prev.follow = this.follow;
   prev.scrollTop = this.thread.scrollTop;
   prev.list.classList.add('is-parked');
   this.resize.unobserve(prev.list);
   // A protected chat locks again as soon as it is left.
   if (prev.record && !prev.locked && this.library.isProtected(prev.id)) this.seal(prev);
  }
  this.attach(conv);
  this.active = conv;
  conv.unread = false;
  conv.list.classList.remove('is-parked');
  this.resize.observe(conv.list);
  const empty = !conv.list.childElementCount;
  this.main.classList.toggle('is-empty', empty && !conv.locked);
  this.follow = conv.follow;
  if (conv.follow) this.pin();
  else this.thread.scrollTop = conv.scrollTop;
  this.lastTop = this.thread.scrollTop;
  this.pinUntil = performance.now() + PIN_TIME;
  if (prev?.list.childElementCount && !empty && !reducedMotion()) conv.list.animate([{ opacity: 0, transform: 'translateY(8px)' }, { opacity: 1, transform: 'none' }], SWITCH);
  this.syncBottom();
 }

 // Holds the end of the chat in view. A card taller than the room over the composer (postStats) holds its own top in view
 // instead, clear of the buttons over the chat.
 pin() {
  const thread = this.thread, anchor = this.anchor?.isConnected ? this.anchor : null;
  let top = thread.scrollHeight;
  if (anchor) top = Math.min(top, anchor.getBoundingClientRect().top - thread.getBoundingClientRect().top + thread.scrollTop - ANCHOR_GAP);
  thread.scrollTop = top;
  this.lastTop = thread.scrollTop;
 }

 stopFollow() {
  cancelAnimationFrame(this.followFrame);
  cancelAnimationFrame(this.jumpFrame);
  this.followFrame = 0;
  this.jumpFrame = 0;
  this.followMoving = false;
 }

 send(text, attachments = []) {
  const conv = this.active, config = this.config(conv);
  if (conv.locked) return false;
  this.anchor = null;
  // With a backend but no model it offers, the settings say how to connect one. With no backend at all the message goes
  // into the chat, and the chat says no backend is connected.
  if (Backend.available && !config.ready) {
   this.settings.open(I18n.t('settings.key.needed'), config.provider);
   return false;
  }
  if (!conv.record) {
   conv.record = this.library.create({ folder: conv.folder, text, attachments });
   this.library.update(conv.id, { model: this.modelOf(conv) });
   this.conversations.set(conv.id, conv);
   this.draft = null;
  } else {
   this.library.update(conv.id, { updated: Date.now() });
  }
  for (const actions of conv.list.querySelectorAll('.message-actions')) actions.remove();
  const prompt = { text, attachments };
  this.follow = true;
  if (conv.turn) {
   this.interject(conv, prompt);
  } else {
   const bubble = this.userMessage(prompt);
   conv.list.append(bubble);
   this.main.classList.remove('is-empty');
   this.run(conv, prompt, config, bubble);
  }
  this.followBottom();
  return true;
 }

 stop() {
  if (this.active?.turn) this.abort(this.active);
 }

 // Stopping ends the turn here at once; the backend is told to stop too, and what it still sends for that turn is ignored.
 abort(conv) {
  const turn = conv.turn;
  if (!turn) return;
  turn.controller.abort();
  turn.release?.('abort');
  if (turn.tool) window.browserPanel?.cancel(turn.tool);
  for (const pending of turn.approvals) pending.card.settle('deny');
  if (turn.remote) Backend.request('turn.cancel', { sessionId: this.sessionOf(conv), turnId: turn.remote }).catch(() => {});
 }

 // The backend weighs pending approvals again against the new mode and answers them itself (approval.resolved).
 onModeChange() {
  if (!Backend.available) return;
  for (const conv of this.conversations.values()) {
   if (conv.turn) Backend.request('session.configure', { sessionId: this.sessionOf(conv), permissionMode: this.settings.mode }).catch(() => {});
  }
 }

 onScroll() {
  const top = this.thread.scrollTop, distance = this.thread.scrollHeight - top - this.thread.clientHeight;
  if (distance <= FOLLOW_DISTANCE) this.follow = true;
  else if (top < this.lastTop - 1) this.follow = false;
  this.lastTop = top;
  this.syncBottom();
 }

 syncBottom() {
  const thread = this.thread, distance = thread.scrollHeight - thread.scrollTop - thread.clientHeight;
  this.bottom.classList.toggle('is-shown', !this.follow && distance > BOTTOM_SHOW);
 }

 scrollToBottom() {
  const thread = this.thread, start = thread.scrollTop;
  this.follow = true;
  this.syncBottom();
  cancelAnimationFrame(this.followFrame);
  cancelAnimationFrame(this.jumpFrame);
  this.followFrame = 0;
  this.followMoving = false;
  const gap = thread.scrollHeight - thread.clientHeight - start;
  // An instant jump notes where it landed, like every animated step does, so scrolling away right after it still lets go.
  if (reducedMotion() || gap < 2) { thread.scrollTop = thread.scrollHeight; this.lastTop = thread.scrollTop; return; }
  const duration = Math.min(JUMP.max, JUMP.base + gap * JUMP.perPixel), begin = performance.now();
  const step = now => {
   this.jumpFrame = 0;
   if (!this.follow) return;
   const p = Math.min(1, Math.max(0, (now - begin) / duration)), max = thread.scrollHeight - thread.clientHeight;
   thread.scrollTop = start + (max - start) * (1 - (1 - p) ** 4);
   this.lastTop = thread.scrollTop;
   if (p < 1) { this.jumpFrame = requestAnimationFrame(step); return; }
   this.followPos = thread.scrollTop;
   this.followBottom();
  };
  this.jumpFrame = requestAnimationFrame(step);
 }

 onDiagramEdit(event) {
  const entry = event.target.closest('.message')?.__entry, { from, to } = event.detail;
  if (!entry || !from || !entry.content.includes(from)) return;
  entry.content = entry.content.replace(from, to);
  const conv = event.target.closest('.thread-list')?.__conversation;
  if (conv?.record && conv.messages.includes(entry)) this.library.saveMessages(conv.id, conv.messages, conv.tokens);
 }

 followBottom() {
  if (!this.follow || this.followFrame || this.jumpFrame) return;
  if (!this.followMoving) {
   this.followPos = this.thread.scrollTop;
   this.followVel = 0;
  }
  this.followLast = performance.now();
  this.followFrame = requestAnimationFrame(this.followStep);
 }

 followStep(now) {
  this.followFrame = 0;
  const thread = this.thread, max = thread.scrollHeight - thread.clientHeight;
  if (!this.follow || reducedMotion()) {
   if (this.follow) { thread.scrollTop = max; this.lastTop = thread.scrollTop; }
   this.followMoving = false;
   return;
  }
  if (Math.abs(thread.scrollTop - this.followPos) > 1.5) {
   this.followPos = thread.scrollTop;
   this.followVel = 0;
  }
  const dt = Math.min(Math.max((now - this.followLast) / 1000, 0), 0.05), [k, c] = FOLLOW_SPRING;
  this.followLast = now;
  const steps = Math.max(1, Math.ceil(dt / 0.004)), h = dt / steps;
  for (let i = 0; i < steps; i++) {
   this.followVel += ((max - this.followPos) * k - this.followVel * c) * h;
   this.followPos += this.followVel * h;
  }
  if (this.followPos >= max) {
   this.followPos = max;
   this.followVel = Math.min(0, this.followVel);
  }
  if (max - this.followPos < 0.5 && Math.abs(this.followVel) < 4) {
   thread.scrollTop = max;
   this.followPos = thread.scrollTop;
   this.lastTop = thread.scrollTop;
   this.followMoving = false;
   return;
  }
  thread.scrollTop = this.followPos;
  this.lastTop = thread.scrollTop;
  this.followMoving = true;
  this.followFrame = requestAnimationFrame(this.followStep);
 }

 async onClick(event) {
  const button = event.target.closest('.md-copy');
  if (!button) return;
  const own = button.classList.contains('message-copy');
  const text = own ? this.copyText(button.closest('.message')) : button.closest('.md-code, .md-calc').querySelector('pre').textContent;
  try {
   await navigator.clipboard.writeText(text);
  } catch {
   return;
  }
  button.classList.add('is-copied');
  button.setAttribute('aria-label', I18n.t('code.copied'));
  clearTimeout(button.copiedTimer);
  button.copiedTimer = setTimeout(() => {
   button.classList.remove('is-copied');
   button.setAttribute('aria-label', I18n.t(own ? 'message.copy' : 'code.copy'));
  }, COPIED_TIME);
 }

 copyText(el) {
  const entry = el.__entry;
  if (!entry?.turn) return entry?.content ?? '';
  const messages = el.closest('.thread-list')?.__conversation?.messages || [];
  const parts = messages.filter(item => item.role === 'assistant' && item.turn === entry.turn && item.content?.trim());
  return parts.length ? parts.map(item => item.content.trim()).join('\n\n') : entry.content;
 }

 // The folder the agent of a chat works in: its project folder, or the chat's own folder when it has none.
 cwd(conv) {
  return conv?.record ? this.library.cwdOf(conv.record) : '';
 }

 // A turn as the chat on screen keeps it. It ends when the backend sends turn.completed (`finish`), or at once when the user
 // stops it.
 begin(conv, config) {
  const turn = conv.turn = {
   id: uid(), remote: '', controller: new AbortController(), config, part: null, parts: [], next: null, queue: [], approvals: new Set(),
   messages: new Set(), requests: new Set(), early: [], prompt: null, compaction: null, tool: '', text: false, finishReason: null,
  };
  turn.done = new Promise((resolve, reject) => {
   turn.finish = ({ status, finishReason, error } = {}) => {
    if (status === 'done') resolve(finishReason || turn.finishReason || null);
    else if (status === 'cancelled') reject(aborted());
    else reject(error instanceof Error ? error : new BackendError(error || {}));
   };
  });
  turn.done.catch(() => {});
  turn.controller.signal.addEventListener('abort', () => turn.finish({ status: 'cancelled' }), { once: true });
  return turn;
 }

 // What the backend gets with every turn besides the message: the model and how hard it thinks, the permission mode, the
 // folder to work in, the user's standing instructions and files, and what the built-in browser holds.
 async sessionParams(conv, turn) {
  await UserContext.ready;
  const config = turn.config;
  return {
   model: config.model, provider: config.provider, thinking: config.effort, permissionMode: this.settings.mode,
   cwd: this.cwd(conv), title: conv.record?.title || '',
   userContext: UserContext.forBackend(),
   host: { browser: window.browserPanel?.snapshot() || null },
  };
 }

 run(conv, prompt, config, bubble) {
  const turn = this.begin(conv, config);
  const entry = turn.prompt = { role: 'user', text: prompt.text, attachments: prompt.attachments.map(slim), content: prompt.text };
  conv.messages.push(entry);
  if (bubble) this.nodes.set(entry, bubble);
  this.openPart(conv, turn);
  this.onChange();
  return this.drive(conv, turn, async () => {
   const input = await inputOf(prompt);
   // A video's place and frame are known only once it is read.
   entry.attachments = prompt.attachments.map(slim);
   return this.startTurn(conv, turn, input);
  });
 }

 async startTurn(conv, turn, input) {
  turn.controller.signal.throwIfAborted();
  const params = { sessionId: this.sessionOf(conv), clientTurnId: turn.id, input, ...(await this.sessionParams(conv, turn)) };
  const result = await Backend.request('turn.start', params, { signal: turn.controller.signal });
  if (result?.turnId) this.setRemote(conv, turn, result.turnId);
  return turn.done;
 }

 resume(conv, config) {
  const turn = this.begin(conv, config);
  this.openPart(conv, turn);
  this.onChange();
  return this.drive(conv, turn, async () => {
   const params = { sessionId: this.sessionOf(conv), clientTurnId: turn.id, ...(await this.sessionParams(conv, turn)) };
   const result = await Backend.request('turn.retry', params, { signal: turn.controller.signal });
   if (result?.turnId) this.setRemote(conv, turn, result.turnId);
   return turn.done;
  });
 }

 // A message sent while a reply is being written waits under it, and goes to the backend to take in between its steps
 // (turn.steer). It joins the chat when the backend says it took it (input.accepted), or after the turn if it never does.
 interject(conv, prompt) {
  const turn = conv.turn, bubble = this.userMessage(prompt);
  if (turn.next) {
   turn.next.el.before(bubble);
  } else {
   turn.next = this.assistantMessage(conv);
   conv.list.append(bubble, turn.next.el);
  }
  const item = { id: uid(), prompt, bubble, input: null };
  turn.queue.push(item);
  this.dismissGhost(turn.part.view);
  for (const pending of turn.approvals) pending.card.settle(SUPERSEDED);
  turn.release?.('message');
  this.steer(conv, turn, item);
 }

 async steer(conv, turn, item) {
  try {
   item.input = await inputOf(item.prompt);
   if (conv.turn !== turn || turn.quiet || !turn.queue.includes(item)) return;
   await turn.started;
   if (conv.turn !== turn || !turn.queue.includes(item)) return;
   await Backend.request('turn.steer', { sessionId: this.sessionOf(conv), turnId: turn.remote, clientInputId: item.id, input: item.input, host: { browser: window.browserPanel?.snapshot() || null } });
  } catch {}
 }

 async drive(conv, turn, work) {
  let error = null, finish = null;
  turn.started = new Promise(resolve => { turn.onStarted = resolve; });
  try {
   finish = await work();
   // Messages sent during a compaction or a model switch start the turn they were waiting for.
   if (turn.quiet && turn.queue.length && conv.turn === turn) finish = await this.continueQueued(conv, turn);
  } catch (e) {
   error = e;
  }
  await this.end(conv, turn, error, finish);
 }

 async continueQueued(conv, turn) {
  const items = turn.queue.splice(0);
  turn.quiet = false;
  turn.config = this.config(conv);
  this.takeQueue(conv, turn, items);
  const inputs = await Promise.all(items.map(item => item.input || inputOf(item.prompt)));
  return this.startTurn(conv, turn, combine(inputs));
 }

 setRemote(conv, turn, id) {
  if (turn.remote || !id) return;
  turn.remote = id;
  turn.onStarted?.();
  for (const [method, params] of turn.early.splice(0)) this.onEvent(conv, method, params);
 }

 // An event from the backend for one of this chat's sessions. Events of a turn that is over, or of another one, are let go.
 onEvent(conv, method, p) {
  if (method === 'session.updated') { this.onSession(conv, p); return; }
  const turn = conv.turn;
  if (!turn) return;
  if (method === 'turn.started') {
   if (p.clientTurnId ? p.clientTurnId === turn.id : !turn.remote) this.setRemote(conv, turn, p.turnId);
   return;
  }
  if (p.turnId) {
   if (!turn.remote) { turn.early.push([method, p]); return; }
   if (p.turnId !== turn.remote) return;
  } else if (p.messageId && method !== 'message.started' && !turn.messages.has(p.messageId)) {
   return;
  }
  switch (method) {
   case 'message.started': this.messageStarted(turn, p); break;
   case 'message.delta': this.messageText(conv, turn, p.messageId, String(p.text ?? ''), false); break;
   case 'message.completed':
    if (typeof p.text === 'string') this.messageText(conv, turn, p.messageId, p.text, true);
    if (p.finishReason) turn.finishReason = p.finishReason;
    break;
   case 'tool.started': this.showGhost(turn.next || turn.part.view); break;
   case 'approval.resolved':
    for (const pending of turn.approvals) if (pending.id === p.approvalId) pending.card.settle(p.decision === 'allow' ? 'allow' : 'deny');
    break;
   case 'compaction.started': this.compactionStarted(conv, turn, p); break;
   case 'compaction.completed': this.compactionDone(conv, turn, !!p.ok); break;
   case 'usage': this.onUsage(conv, turn, p); break;
   case 'input.accepted': {
    const item = turn.queue.find(entry => entry.id === p.clientInputId);
    if (item) this.takeQueue(conv, turn, [item]);
    break;
   }
   case 'turn.completed': turn.finish(p); break;
   default: break;
  }
 }

 // Replies come in parts: a part holds every message of the reply until a message the user sent in between, or a
 // compaction, starts the next one.
 messageStarted(turn, p) {
  if (p.messageId) turn.messages.add(p.messageId);
  const part = turn.part;
  part.base = part.entry.content;
  part.text = '';
 }

 messageText(conv, turn, messageId, text, whole) {
  if (messageId && !turn.messages.has(messageId)) return;
  const part = turn.part, view = part.view;
  part.text = whole ? text : (part.text || '') + text;
  if (!part.text.trim()) return;
  turn.text = true;
  part.entry.content = join(part.base || '', part.text);
  this.dismissGhost(view);
  view.stream.push(part.entry.content);
 }

 onUsage(conv, turn, usage) {
  Usage.record(usage);
  spend(turn.compaction?.entry || turn.part.entry, usage);
  if (usage.context) {
   conv.tokens = Number(usage.context.used) || 0;
   conv.window = Number(usage.context.window) || 0;
  }
 }

 // The backend names the chat; a chat renamed by hand keeps the user's name.
 onSession(conv, p) {
  const record = conv.record && this.library.chat(conv.id);
  const title = typeof p.title === 'string' ? p.title.replace(/\s+/g, ' ').trim().slice(0, 60) : '';
  if (record && title && !record.renamed) this.library.update(conv.id, { title, named: true });
 }

 // A request the backend makes for a step (approval.request, host.tool) belongs to the turn it names. It gets that turn
 // only while it is the chat's running turn: one that comes before turn.start is answered waits for the turn's id, one
 // for a turn that ended or was stopped gets null (answered as cancelled), and one for another turn, or a step asked
 // twice, is refused with `stale_turn` before anything shows or runs.
 async claim(conv, p, signal, key) {
  const turn = conv.turn;
  if (turn && !turn.remote && !turn.controller.signal.aborted && !signal.aborted) {
   await Promise.race([turn.started, turn.done, new Promise(resolve => signal.addEventListener('abort', resolve, { once: true }))]).catch(() => {});
  }
  if (!turn || conv.turn !== turn || turn.controller.signal.aborted || signal.aborted) return null;
  const stale = message => new BackendError({ code: 'stale_turn', message });
  if (!p.turnId || p.turnId !== turn.remote) throw stale(`Turn ${p.turnId || '(none)'} is not the running turn of session ${p.sessionId}`);
  if (key) {
   if (turn.requests.has(key)) throw stale(`${key} was already asked in turn ${p.turnId}`);
   turn.requests.add(key);
  }
  return turn;
 }

 // The backend asks before a step: the card goes under the reply, and its answer goes back. A message sent instead
 // answers it too.
 async onApproval(conv, p, signal) {
  const turn = await this.claim(conv, p, signal, p.approvalId && `approval ${p.approvalId}`);
  if (!turn) return { decision: 'deny', reason: 'cancelled' };
  if (turn.queue.length) return { decision: 'deny', reason: SUPERSEDED };
  const view = turn.part.view;
  this.dismissGhost(view);
  const card = new ApprovalCard(ApprovalCard.present(p.presentation, p.tool, p.args));
  const pending = { id: p.approvalId, card };
  view.el.append(card.el);
  turn.approvals.add(pending);
  signal.addEventListener('abort', () => card.settle('deny'), { once: true });
  if (conv === this.active) this.followBottom();
  const answer = await card.answer;
  turn.approvals.delete(pending);
  card.dismiss();
  if (answer === 'allow') return { decision: 'allow' };
  if (answer === SUPERSEDED) return { decision: 'deny', reason: SUPERSEDED };
  return { decision: 'deny', ...(turn.controller.signal.aborted || signal.aborted ? { reason: 'cancelled' } : {}) };
 }

 // The backend asks the app to run one of its host tools: the built-in browser. While the user has taken control of the
 // browser the step waits for them to hand it back, then reports what the page is now instead.
 async onHostTool(conv, p, signal) {
  const panel = window.browserPanel;
  if (!panel || !window.HostTools?.has(p.name)) throw new BackendError({ code: 'unsupported', message: `The app has no host tool ${p.name}` });
  const turn = await this.claim(conv, p, signal, p.toolCallId && `tool call ${p.toolCallId}`);
  if (!turn) return { status: 'cancelled', content: [] };
  this.showGhost(turn.next || turn.part.view);
  panel.drive(conv, true);
  let handed = false;
  if (panel.userHas) {
   const why = await new Promise(resolve => {
    turn.release = resolve;
    panel.waitForAgent().then(() => resolve('back'));
    signal.addEventListener('abort', () => resolve('abort'), { once: true });
   });
   turn.release = null;
   if (why === 'abort' || signal.aborted || turn.controller.signal.aborted || conv.turn !== turn) return { status: 'cancelled', content: [] };
   if (why === 'message') return { status: 'cancelled', reason: 'message', content: [] };
   handed = true;
  }
  const id = turn.tool = `${conv.id}-${++this.tools}`;
  const stop = () => panel.cancel(id);
  signal.addEventListener('abort', stop, { once: true });
  try {
   const answer = await panel.run(handed ? 'browser_snapshot' : p.name, handed ? {} : p.args || {}, { id });
   if (signal.aborted || turn.controller.signal.aborted || conv.turn !== turn) return { status: 'cancelled', content: [] };
   const result = HostTools.result(p.args || {}, answer);
   return { ...result, status: handed ? 'handed-back' : result.isError ? 'error' : 'ok' };
  } finally {
   signal.removeEventListener('abort', stop);
   if (turn.tool === id) turn.tool = '';
  }
 }

 // The messages the backend took in between its steps join the chat, and the reply goes on in a new part under them.
 takeQueue(conv, turn, items) {
  for (const item of items) {
   const at = turn.queue.indexOf(item);
   if (at >= 0) turn.queue.splice(at, 1);
  }
  this.closePart(conv, turn.part);
  for (const { prompt, bubble } of items) {
   const entry = { role: 'user', text: prompt.text, attachments: prompt.attachments.map(slim), content: prompt.text };
   conv.messages.push(entry);
   this.nodes.set(entry, bubble);
  }
  this.openPart(conv, turn, turn.next);
  turn.next = null;
 }

 openPart(conv, turn, view = null) {
  view ||= this.assistantMessage(conv);
  if (!view.el.isConnected) conv.list.append(view.el);
  // Each part of a reply notes the model that wrote it, for the chat's stats and for a model that later takes over.
  const entry = { role: 'assistant', content: '', turn: turn.id, model: turn.config.id };
  conv.messages.push(entry);
  view.el.__entry = entry;
  turn.part = { view, entry, base: '', text: '' };
  turn.parts.push(turn.part);
 }

 closePart(conv, { view, entry }) {
  this.dismissGhost(view);
  if (!entry.content && !entry.usage) drop(conv.messages, entry);
  view.stream.finish().then(() => {
   view.el.classList.remove('is-streaming');
   if (!entry.content.trim()) collapse(view.el);
  });
 }

 async end(conv, turn, error, finish) {
  if (conv.turn !== turn) return;
  const { view, entry } = turn.part, aborted = error?.name === 'AbortError';
  for (const pending of turn.approvals) pending.card.settle('deny');
  // A browser step the turn still has running, or waiting for the user to hand the browser back, ends with it.
  turn.release?.('abort');
  if (turn.tool) window.browserPanel?.cancel(turn.tool);
  conv.turn = null;
  if (turn.compaction) this.compactionDone(conv, turn, false);
  else if (error && !aborted && (turn.switch || turn.compacting)) this.failedNotice(conv, turn);
  if (turn.switch && this.library.chat(conv.id)) {
   this.library.update(conv.id, { model: turn.switch });
   this.settings.setModel(turn.switch);
  }
  window.browserPanel?.drive(conv, false);
  if (!entry.content && !entry.usage) drop(conv.messages, entry);
  if (turn.next) collapse(turn.next.el);
  // Messages the backend never took stay in the chat as they were sent.
  for (const { prompt, bubble } of turn.queue.splice(0)) {
   const item = { role: 'user', text: prompt.text, attachments: prompt.attachments.map(slim), content: prompt.text };
   conv.messages.push(item);
   this.nodes.set(item, bubble);
  }
  if (conv.record && this.library.chat(conv.id)) this.save(conv);
  if (conv !== this.active) conv.unread = true;
  this.dismissGhost(view);
  this.onChange();
  await view.stream.finish();
  view.el.classList.remove('is-streaming');
  const text = !!entry.content.trim();
  let noted = true;
  if (error && !aborted && !turn.quiet) this.fail(conv, view, error);
  else if (turn.quiet) noted = false;
  else if (aborted) this.note(view, I18n.t('chat.stopped'));
  else if (FINISH_NOTES.includes(finish)) this.note(view, I18n.t(`finish.${finish}`));
  else if (!turn.text) this.note(view, I18n.t('chat.empty'));
  else noted = false;
  const last = text ? view : turn.parts.findLast(item => item.entry.content.trim() && item.view.el.isConnected)?.view;
  if (last) {
   const tools = this.toolbar(), box = last === view && view.el.querySelector('.message-error, .message-note');
   if (box) box.before(tools);
   else last.el.append(tools);
  }
  if (!text && !noted) collapse(view.el);
  if (conv === this.active) this.followBottom();
  // A protected chat left while it was replying locks fully once the reply is saved.
  if (conv.locked && !conv.turn) this.seal(conv);
 }

 switchLabels(from, to) {
  const name = id => this.settings.find(id)?.name || id;
  return {
   running: I18n.t('compact.switch.running', { name: name(to) }),
   done: I18n.t('compact.switch.done', { name: name(to) }),
   failed: I18n.t('compact.switch.failed', { name: name(from) }),
  };
 }

 // The backend compacts the chat: a line says so where the summary takes the place of what came before. At the start of
 // a turn that is over the message just sent; in the middle of one, under the reply so far, which goes on below it.
 compactionStarted(conv, turn, p) {
  if (turn.compaction) return;
  const labels = turn.switch ? this.switchLabels(turn.switch, this.modelOf(conv)) : null;
  const notice = this.compactNotice(true, labels), part = turn.part;
  const before = turn.parts.length === 1 && !part.entry.content && turn.prompt && this.nodes.get(turn.prompt);
  let at = null;
  if (part.entry.content) {
   this.closePart(conv, part);
   conv.list.append(notice);
   turn.reopen = true;
  } else if (before?.isConnected) {
   before.before(notice);
   at = turn.prompt;
  } else {
   part.view.el.before(notice);
   at = part.entry;
  }
  turn.compaction = { notice, at, entry: { role: 'compact', model: turn.config.id } };
  if (conv === this.active) this.followBottom();
 }

 compactionDone(conv, turn, ok) {
  const compaction = turn.compaction;
  if (!compaction) return;
  turn.compaction = null;
  this.finishNotice(compaction.notice, ok);
  if (ok) {
   const at = compaction.at ? conv.messages.indexOf(compaction.at) : -1;
   conv.messages.splice(at < 0 ? conv.messages.length : at, 0, compaction.entry);
   this.nodes.set(compaction.entry, compaction.notice);
  }
  if (turn.reopen && conv.turn === turn) {
   turn.reopen = false;
   this.openPart(conv, turn);
  }
  if (conv.record && this.library.chat(conv.id)) this.save(conv);
 }

 // A compaction or a model switch that failed before the backend even began says so with the same line.
 failedNotice(conv, turn) {
  const notice = this.compactNotice(true, turn.switch ? this.switchLabels(turn.switch, this.modelOf(conv)) : null);
  conv.list.append(notice);
  this.finishNotice(notice, false);
 }

 compactNotice(live, labels = null) {
  const el = document.createElement('div');
  el.className = `thread-compact${live ? ' is-live' : ''}`;
  el.labels = labels;
  const text = document.createElement('span');
  text.className = 'thread-compact-text';
  text.textContent = live ? labels?.running || I18n.t('compact.running') : I18n.t('compact.done');
  el.append(text);
  return el;
 }

 finishNotice(el, ok) {
  const text = el.querySelector('.thread-compact-text');
  el.classList.remove('is-live');
  text.textContent = ok ? el.labels?.done || I18n.t('compact.done') : el.labels?.failed || I18n.t('compact.failed');
  if (!reducedMotion()) text.animate([{ opacity: 0, filter: 'blur(3px)' }, { opacity: 1, filter: 'blur(0)' }], { duration: 360, easing: 'ease-out' });
 }

 save(conv) {
  if (!this.library.chat(conv.id)) return;
  this.library.saveMessages(conv.id, conv.messages, conv.tokens);
  this.library.update(conv.id, { updated: Date.now() });
 }

 // What a chat has spent, per model and per reply, with its mini chat and how full its context is. Only what was counted is
 // here: replies written before the app kept count are only numbered.
 async stats(conv = this.active) {
  const models = new Map(), turns = new Map(), uncounted = new Set();
  for (const entry of conv.messages) {
   if (entry.role !== 'assistant' && entry.role !== 'compact') continue;
   const turn = entry.role === 'assistant' ? entry.turn || entry : null;
   if (!entry.usage) {
    if (turn && entry.steps?.length) uncounted.add(turn);
    continue;
   }
   const model = entry.model || this.modelOf(conv);
   if (!models.has(model)) models.set(model, tokens());
   addUp(models.get(model), entry.usage);
   if (!turn) continue;
   if (!turns.has(turn)) turns.set(turn, { model, spent: tokens() });
   addUp(turns.get(turn).spent, entry.usage);
  }
  const side = await this.library.side?.(conv.id);
  let mini = null;
  for (const entry of side?.messages || []) if (entry.usage && (entry.role === 'assistant' || entry.role === 'compact')) addUp(mini ||= tokens(), entry.usage);
  const order = [...models.keys()];
  return {
   version: 1,
   models: order.map(id => ({ id, name: this.settings.find(id)?.name || String(id).split(':').pop(), ...models.get(id) })),
   turns: [...turns.values()].map(({ model, spent }) => ({ m: order.indexOf(model), t: spent.input + spent.output, c: spent.cached })),
   mini,
   context: { used: conv.tokens || 0, window: conv.window || this.settings.windowOf(this.modelOf(conv)) },
   uncounted: uncounted.size,
  };
 }

 // Puts the chat's numbers into it as a card. The card is the user's only: it is kept with the chat, but no model ever sees it.
 async postStats(from = null) {
  const conv = this.active;
  if (!this.canStats) return false;
  const stats = await this.stats(conv);
  if (conv !== this.active || conv.turn) return false;
  const entry = { role: 'stats', stats };
  conv.messages.push(entry);
  const el = this.entryView(entry);
  this.nodes.set(entry, el);
  this.main.classList.remove('is-empty');
  this.follow = true;
  this.pinUntil = performance.now() + PIN_TIME;
  this.anchor = el;
  setTimeout(() => { if (this.anchor === el) this.anchor = null; }, PIN_TIME);
  conv.list.append(el);
  StatsCard.enter(el, { from, pin: () => this.pin() });
  // Saved without moving the chat up the list: nothing was said in it.
  this.library.saveMessages(conv.id, conv.messages, conv.tokens);
  return true;
 }

 onStatsRemove(event) {
  const el = event.target.closest('.stats-item'), entry = el?.__entry, conv = el?.closest('.thread-list')?.__conversation;
  if (!entry || !conv) return;
  drop(conv.messages, entry);
  // A card taken away right after it came in no longer holds the chat's view.
  if (this.anchor === el) this.anchor = null;
  StatsCard.leave(el);
  if (conv.record && this.library.chat(conv.id)) this.library.saveMessages(conv.id, conv.messages, conv.tokens);
 }

 restore(conv) {
  this.attach(conv);
  const ends = new Map();
  for (const entry of conv.messages) if (entry.role === 'assistant' && entry.content?.trim()) ends.set(entry.turn || entry, entry);
  for (const entry of conv.messages) {
   const el = this.entryView(entry, ends.get(entry.turn || entry) === entry);
   if (!el) continue;
   conv.list.append(el);
   this.nodes.set(entry, el);
  }
  settle(conv.list);
 }

 // What a saved entry shows as when its chat opens; `last` marks the reply that ends its turn.
 entryView(entry, last) {
  if (entry.role === 'user') return this.userMessage(this.promptOf(entry));
  if (entry.role === 'compact') return this.compactNotice(false);
  if (entry.role === 'stats') {
   const el = StatsCard.build(entry.stats);
   el.__entry = entry;
   return el;
  }
  if (entry.content?.trim()) return this.restoredMessage(entry, last);
  return null;
 }

 promptOf(entry) {
  // A picture is kept with its message; chats from before 1.3.0's frontend-only build kept it in what the model was sent.
  const urls = Array.isArray(entry.content) ? entry.content.filter(part => part.type === 'image_url').map(part => part.image_url.url) : [];
  let k = 0;
  const attachments = (entry.attachments || []).map(item => ({
   ...item, info: FileKinds.describe(item.name), url: item.image ? item.url || urls[k++] || '' : item.video?.poster || '', duration: item.video?.duration || 0,
  }));
  return { text: entry.text || '', attachments };
 }

 restoredMessage(entry, last = true) {
  const el = document.createElement('div');
  el.className = 'message is-assistant';
  const content = document.createElement('div');
  content.className = 'message-content markdown';
  el.append(content);
  StreamView.render(content, entry.content);
  el.__entry = entry;
  if (last) el.append(this.toolbar());
  return el;
 }

 toolbar() {
  const tools = document.createElement('div');
  tools.className = 'message-tools';
  tools.innerHTML = `<button class="md-copy message-copy" type="button" aria-label="${I18n.t('message.copy')}">${Markdown.COPY_ICON}</button>`;
  return tools;
 }

 showGhost(view) {
  const current = view.status;
  if (current?.isConnected && !current.classList.contains('is-leaving')) {
   if (current !== view.el.lastElementChild && view.content.hasChildNodes()) view.el.append(current);
   return;
  }
  const status = document.createElement('div');
  status.className = 'message-status is-working';
  status.innerHTML = '<ghost-thinking></ghost-thinking>';
  view.el.append(status);
  view.status = status;
  if (view.el.closest('.thread-list') === this.active?.list) this.followBottom();
 }

 dismissGhost(view) {
  const status = view.status;
  if (!status?.isConnected || status.classList.contains('is-leaving')) return;
  status.classList.add('is-leaving');
  if (reducedMotion()) { status.remove(); return; }
  const style = getComputedStyle(status);
  status.animate([
   { height: `${status.offsetHeight}px`, paddingTop: style.paddingTop, opacity: 1, transform: 'none' },
   { height: '0px', paddingTop: '0px', opacity: 0, transform: 'scale(0.7)' },
  ], LEAVE).finished.then(() => status.remove());
 }

 fail(conv, view, error) {
  const { message, settings } = Backend.explain(error);
  const box = document.createElement('div');
  box.className = 'message-error';
  box.textContent = message;
  const actions = document.createElement('div');
  actions.className = 'message-actions';
  if (settings) actions.append(this.action(I18n.t('chat.open-settings'), () => this.settings.open()));
  actions.append(this.action(I18n.t('chat.retry'), () => this.retry(conv, view)));
  view.el.append(box, actions);
 }

 retry(conv, view) {
  if (conv.turn) return;
  const config = this.config(conv);
  if (Backend.available && !config.ready) {
   this.settings.open(I18n.t('settings.key.needed'), config.provider);
   return;
  }
  for (const node of view.el.querySelectorAll('.message-error, .message-actions')) node.remove();
  if (!conv.messages.includes(view.el.__entry)) view.el.remove();
  if (conv === this.active) this.follow = true;
  this.resume(conv, config);
  if (conv === this.active) this.followBottom();
 }

 note(view, text) {
  const note = document.createElement('div');
  note.className = 'message-note';
  note.textContent = text;
  view.el.append(note);
 }

 action(label, onClick) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'message-action';
  button.textContent = label;
  button.addEventListener('click', onClick);
  return button;
 }

 userMessage({ text, attachments }) {
  const el = document.createElement('div');
  el.className = 'message is-user';
  // A video sent the moment it was added may still be having its frame taken: the attachments come in once it is there.
  if (attachments.some(item => item.info?.glyph === 'video' && item.ready && !item.payload)) {
   Promise.all(attachments.map(item => item.ready)).then(() => el.prepend(...this.attachmentViews(attachments)));
  } else {
   el.append(...this.attachmentViews(attachments));
  }
  const { quotes, rest } = splitQuotes(text);
  for (const quote of quotes) {
   const box = document.createElement('div');
   box.className = 'message-quote';
   box.innerHTML = Glyphs.quote;
   const body = document.createElement('span');
   body.className = 'message-quote-text';
   body.textContent = quote;
   box.title = quote;
   box.append(body);
   el.append(box);
  }
  if (rest) {
   const bubble = document.createElement('div');
   bubble.className = 'message-bubble';
   LinkChip.fill(bubble, rest);
   el.append(bubble);
  }
  if (text) {
   el.append(this.toolbar());
   el.__entry = { content: text };
  }
  return el;
 }

 // Photos and videos with a frame go into the stack of pictures; everything else is a file's card.
 attachmentViews(attachments) {
  const views = [], media = attachments.filter(item => (item.image || item.video) && item.url), files = attachments.filter(item => !item.image && !media.includes(item));
  if (media.length) views.push(new MediaSlider(media.map(({ url, width, height, name, note, video, duration }) => ({ url, width, height, name, note, video: !!video, duration }))).el);
  if (files.length) {
   const box = document.createElement('div');
   box.className = 'message-files';
   for (const item of files) box.append(this.fileCard(item));
   views.push(box);
  }
  return views;
 }

 fileCard(item) {
  const card = document.createElement('div');
  card.className = 'file-card';
  card.title = item.pasted?.preview || item.name;
  card.innerHTML = FileKinds.icon(item.info);
  const text = document.createElement('div');
  text.className = 'file-card-text';
  const name = document.createElement('div');
  name.className = 'file-card-name';
  name.textContent = item.pasted?.preview || item.name;
  const meta = document.createElement('div');
  meta.className = 'file-card-meta';
  meta.textContent = item.pasted ? Attachments.pastedLabel(item.pasted, item.size)
   : [item.info.name, item.duration ? FileKinds.formatDuration(item.duration) : '', FileKinds.formatSize(item.size)].filter(Boolean).join(' · ');
  text.append(name, meta);
  if (item.note) {
   const note = document.createElement('div');
   note.className = 'file-card-note';
   note.textContent = item.note;
   text.append(note);
  }
  card.append(text);
  return card;
 }

 assistantMessage(conv) {
  const el = document.createElement('div');
  el.className = 'message is-assistant is-streaming';
  el.innerHTML = '<div class="message-status"><ghost-thinking></ghost-thinking></div><div class="message-content markdown"></div>';
  const content = el.querySelector('.message-content');
  return { el, status: el.querySelector('.message-status'), content, stream: new StreamView(content, { onChange: () => { if (conv === this.active) this.followBottom(); } }) };
 }
}

function movedNotice() {
 const el = document.createElement('div');
 el.className = 'thread-compact thread-moved';
 const text = document.createElement('span');
 text.className = 'thread-compact-text';
 text.textContent = I18n.t('mini.moved');
 el.append(text);
 return el;
}

// The mini chat over a chat. It keeps its own messages with that chat, in a side session of the backend's that reads the
// chat it hangs off (side.parent in turn.start); what the side session tells its model is the backend's business. Where
// the chat has moved on since the last question, a line in the mini chat says so.
class SideChat extends Chat {
 constructor({ library, origin, model, ...options }) {
  const state = { seen: 0 };
  const record = { id: origin.id, title: '', folder: origin.record.folder, created: 0, updated: 0, pinned: false, named: true, model };
  if (library.isHome(origin.record)) record.space = origin.record.space;
  super({ ...options, library: {
   folders: [],
   chats: [record],
   chat: id => id === record.id ? record : null,
   update: (id, changes) => id === record.id ? Object.assign(record, changes) : null,
   conversation: id => library.side(id).then(body => { state.seen = body.seen; return body; }),
   saveMessages: (id, messages, tokens) => library.saveSide(id, { messages, tokens, seen: state.seen }),
   clear: id => library.clearSide(id),
   isProtected: () => false,
   isLocked: () => false,
   relock() {},
   isHome: chat => library.isHome(chat),
   cwdOf: chat => library.cwdOf(chat),
  } });
  this.state = state;
  this.origin = origin;
  this.waiting = null;
  this.driving = null;
 }

 get hasMessages() {
  return !!this.active?.record && this.active.messages.some(entry => entry.role === 'user');
 }

 // The chat has changed since the latest question asked here.
 get behind() {
  return this.hasMessages && (this.origin.record?.updated || 0) > this.state.seen;
 }

 // Opens the mini chat's own messages, and where the chat has moved on since, a line says the mini chat caught up with it.
 async start() {
  await this.open(this.origin.id);
  if (!this.behind) return;
  this.waiting = movedNotice();
  this.active.list.append(this.waiting);
  this.pin();
 }

 send(text, attachments = []) {
  const conv = this.active;
  if (!conv?.record) return false;
  if (conv.turn) return super.send(text, attachments);
  const mark = this.behind ? { role: 'moved' } : null, seen = this.state.seen;
  if (mark) {
   conv.messages.push(mark);
   if (!this.waiting) conv.list.append(this.waiting = movedNotice());
  }
  this.state.seen = this.origin.record?.updated || 0;
  this.moved = !!mark;
  if (!super.send(text, attachments)) {
   if (mark) drop(conv.messages, mark);
   this.state.seen = seen;
   return false;
  }
  if (mark) this.nodes.set(mark, this.waiting);
  this.waiting = null;
  return true;
 }

 sessionOf(conv) {
  return conv?.id ? `${conv.id}:mini` : '';
 }

 async sessionParams(conv, turn) {
  const params = await super.sessionParams(conv, turn);
  // `moved`: the chat has moved on since the mini chat's last question.
  const moved = !!this.moved;
  this.moved = false;
  return { ...params, side: { parent: this.origin.id, parentBusy: !!this.origin.turn, moved } };
 }

 entryView(entry, last) {
  return entry.role === 'moved' ? movedNotice() : super.entryView(entry, last);
 }

 run(conv, prompt, config, bubble) {
  return this.driving = super.run(conv, prompt, config, bubble);
 }

 resume(conv, config) {
  return this.driving = super.resume(conv, config);
 }

 // Resolves once no reply is being written here and what the last one wrote is on its way to the disk.
 idle() {
  return Promise.resolve(this.driving).catch(() => {});
 }

 // Starts the mini chat over: its messages go, from the screen and from the disk.
 clear() {
  const conv = this.active;
  if (!conv?.record) return Promise.resolve();
  this.abort(conv);
  if (Backend.can('sessions.delete')) Backend.request('session.delete', { sessionId: this.sessionOf(conv) }).catch(() => {});
  conv.messages = [];
  conv.tokens = 0;
  this.state.seen = 0;
  this.waiting = null;
  conv.list.replaceChildren();
  this.main.classList.add('is-empty');
  this.syncBottom();
  this.onChange();
  return this.library.clear(conv.id);
 }
}

// Every event and request from the backend names its session; it goes to the chat that holds that session, the newest
// window first, so a mini chat opened again over the same chat is the one that hears it.
function owner(sessionId) {
 for (const chat of [...chats].reverse()) {
  const conv = chat.bySession(sessionId);
  if (conv) return { chat, conv };
 }
 return null;
}

function unknown(sessionId) {
 return new BackendError({ code: 'unknown_session', message: `The app has no session ${sessionId}` });
}

Backend.on('*', (method, params) => {
 const found = owner(params?.sessionId);
 if (found) found.chat.onEvent(found.conv, method, params || {});
 else if (method === 'log') console[params?.level === 'error' ? 'error' : 'log']('[backend]', params?.message);
});
Backend.handle('approval.request', (params, { signal }) => {
 const found = owner(params.sessionId);
 if (!found) throw unknown(params.sessionId);
 return found.chat.onApproval(found.conv, params, signal);
});
Backend.handle('host.tool', (params, { signal }) => {
 const found = owner(params.sessionId);
 if (!found) throw unknown(params.sessionId);
 return found.chat.onHostTool(found.conv, params, signal);
});
// A backend that goes away mid-reply ends every reply being written with an error, and the chat says so.
Backend.on('closed', () => {
 for (const chat of chats) {
  for (const conv of chat.conversations.values()) conv.turn?.finish({ status: 'error', error: { code: 'backend_crashed', message: '' } });
 }
});

window.Chat = Chat;
window.SideChat = SideChat;
})();
