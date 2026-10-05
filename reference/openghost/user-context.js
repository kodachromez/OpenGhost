(() => {
'use strict';

// What the user tells OpenGhost once, in Settings → General, for every chat: standing instructions and files to keep at hand.
// They are kept here and go to the backend with every turn (userContext in turn.start); how they reach the model is the
// backend's business.
const KEY = 'context';
const SAVE_DELAY = 400;
// Local storage/payload guardrails, not model context or token limits. AttachmentReader also bounds individual files.
const LIMITS = { instructions: 8000, files: 20, textChars: 200000 };

const uid = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
const textChars = file => file.kind === 'text' ? file.chars : 0;
const same = (a, b) => a.path && b.path ? Library.samePath(a.path, b.path) : a.name === b.name && a.size === b.size;

class UserContext {
 constructor(store) {
  this.store = store;
  this.instructions = '';
  this.files = [];
  this.payloads = new Map();
  this.listeners = new Set();
  this.timer = 0;
  this.ready = this.load();
 }

 async load() {
  const saved = await this.store.read(KEY).catch(() => null);
  this.instructions = typeof saved?.instructions === 'string' ? saved.instructions : '';
  const files = (Array.isArray(saved?.files) ? saved.files : []).filter(file => file?.id && file.name && ['text', 'image', 'none'].includes(file.kind));
  await Promise.all(files.map(async file => {
   if (file.kind === 'none') return;
   const payload = await this.store.read(`${KEY}/${file.id}`).catch(() => null);
   if (payload?.text != null || payload?.url) this.payloads.set(file.id, payload);
  }));
  this.files = files.filter(file => file.kind === 'none' || this.payloads.has(file.id));
  this.emit();
 }

 get limits() {
  return LIMITS;
 }

 textChars() {
  return this.files.reduce((sum, file) => sum + textChars(file), 0);
 }

 onChange(listener) {
  this.listeners.add(listener);
  return () => this.listeners.delete(listener);
 }

 emit() {
  for (const listener of this.listeners) listener();
 }

 setInstructions(text) {
  const next = String(text).slice(0, LIMITS.instructions);
  if (next === this.instructions) return;
  this.instructions = next;
  this.save();
 }

 save(now = false) {
  clearTimeout(this.timer);
  this.timer = 0;
  const write = () => this.store.write(KEY, { version: 1, instructions: this.instructions, files: this.files }).catch(() => {});
  if (now) write();
  else this.timer = setTimeout(write, SAVE_DELAY);
 }

 // Writes a pending change at once, when the window goes away.
 flush() {
  if (this.timer) this.save(true);
 }

 // Reads files the same way chat attachments are read. The same file added again replaces the old copy, which is how
 // a changed file gets updated. Answers with the files that could not be added and why.
 async add(list) {
  const skipped = [];
  for (const file of Array.from(list || [])) {
   const name = file.name || 'file';
   const info = FileKinds.describe(name, file.type);
   const payload = await AttachmentReader.read(file, info);
   const entry = { id: uid(), name, size: file.size, kind: payload.type, path: window.openghost?.pathOf?.(file) || '', added: Date.now() };
   if (payload.type === 'text') Object.assign(entry, { chars: payload.text.length, truncated: payload.truncated });
   if (payload.type === 'image') Object.assign(entry, { width: payload.width, height: payload.height });
   // Without a path there is nothing the agent could open later, so an unreadable file is of no use.
   if (payload.type === 'none' && !entry.path) { skipped.push({ name, reason: 'unreadable' }); continue; }
   const old = this.files.find(item => same(item, entry));
   if (!old && this.files.length >= LIMITS.files) { skipped.push({ name, reason: 'count' }); continue; }
   if (this.textChars() - (old ? textChars(old) : 0) + textChars(entry) > LIMITS.textChars) { skipped.push({ name, reason: 'size' }); continue; }
   if (payload.type !== 'none') {
    const kept = payload.type === 'text' ? { text: payload.text } : { url: payload.url };
    this.payloads.set(entry.id, kept);
    await this.store.write(`${KEY}/${entry.id}`, kept).catch(() => {});
   }
   if (old) {
    this.files.splice(this.files.indexOf(old), 1, entry);
    this.drop(old.id);
   } else {
    this.files.push(entry);
   }
  }
  this.save(true);
  this.emit();
  return skipped;
 }

 remove(id) {
  const at = this.files.findIndex(file => file.id === id);
  if (at < 0) return;
  this.files.splice(at, 1);
  this.drop(id);
  this.save(true);
  this.emit();
 }

 drop(id) {
  this.payloads.delete(id);
  this.store.remove(`${KEY}/${id}`).catch(() => {});
 }

 // The instructions and files as the backend gets them (ABP UserContext): the text of a text file, the picture of an image,
 // and for any file its place on the disk when it has one.
 forBackend() {
  return {
   instructions: this.instructions.trim(),
   files: this.files.map(file => {
    const payload = this.payloads.get(file.id) || {};
    return {
     id: file.id, name: file.name, size: file.size, kind: file.kind === 'none' ? 'file' : file.kind,
     ...(file.path ? { path: file.path } : {}),
     ...(file.kind === 'text' ? { text: payload.text ?? '', truncated: !!file.truncated } : {}),
     ...(file.kind === 'image' ? { dataUrl: payload.url || '', width: file.width, height: file.height } : {}),
    };
   }),
  };
 }
}

window.UserContext = new UserContext(window.ChatStore);
})();
