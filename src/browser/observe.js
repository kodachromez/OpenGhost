// Adapted from reference/openghost/desktop/browser.js and host-tools.js.
// OpenGhost attribution: NOTICE.md. Runs only in the guest ApplicationWorld.
function install(pageId) {
 if (window.__og?.pageId === pageId) return;
 const ACTIVE = new Set(['link', 'button', 'combobox', 'listbox', 'textbox', 'searchbox', 'checkbox', 'radio', 'slider', 'spinbutton', 'switch', 'tab', 'menuitem', 'menuitemcheckbox', 'menuitemradio', 'option', 'treeitem', 'file input', 'clickable']);
 const TYPING = new Set(['textbox', 'searchbox', 'combobox', 'spinbutton']);
 const INPUTS = { button: 'button', submit: 'button', reset: 'button', image: 'button', checkbox: 'checkbox', radio: 'radio', range: 'slider', search: 'searchbox', file: 'file input', number: 'spinbutton', hidden: '' };
 const SKIP = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'HEAD', 'META', 'LINK', 'svg', 'SVG', 'CANVAS', 'VIDEO', 'AUDIO', 'OBJECT', 'EMBED']);
 const INLINE = new Set(['SPAN', 'B', 'I', 'EM', 'STRONG', 'SMALL', 'MARK', 'CODE', 'SUB', 'SUP', 'ABBR', 'TIME', 'FONT', 'U', 'S', 'Q', 'CITE', 'BR', 'WBR', 'DATA', 'VAR', 'KBD', 'SAMP', 'BDI', 'BDO']);
 const SELECTOR = 'a[href],button,input:not([type=hidden]),select,textarea,summary,[role=button],[role=link],[role=checkbox],[role=radio],[role=tab],[role=menuitem],[role=option],[role=switch],[role=combobox],[role=textbox],[role=searchbox],[contenteditable=""],[contenteditable=true],[onclick]';
 const state = { next: 1, refs: new Map(), ids: new WeakMap() };
 const clean = text => (text || '').replace(/\s+/g, ' ').trim();
 const cut = (text, max) => text.length > max ? `${text.slice(0, max - 1)}…` : text;
 const quote = (text, max = 100) => `"${cut(clean(text), max).replace(/"/g, '\'')}"`;
 const styleOf = el => el.ownerDocument.defaultView.getComputedStyle(el);
 const shown = el => { const style = styleOf(el); return style.visibility !== 'hidden' && style.display !== 'none' && Number(style.opacity) > 0.02; };

 function rectOf(el) {
  const r = el.getBoundingClientRect();
  let x = 0, y = 0, view = el.ownerDocument.defaultView;
  while (view && view.frameElement) {
   const f = view.frameElement.getBoundingClientRect();
   x += f.left + view.frameElement.clientLeft;
   y += f.top + view.frameElement.clientTop;
   view = view.parent;
  }
  return { left: r.left + x, top: r.top + y, right: r.right + x, bottom: r.bottom + y, width: r.width, height: r.height };
 }

 function roleOf(el) {
  const role = el.getAttribute('role');
  if (role) return role.split(/\s+/)[0];
  const tag = el.tagName;
  if (tag === 'A') return el.hasAttribute('href') ? 'link' : '';
  if (tag === 'BUTTON' || tag === 'SUMMARY') return 'button';
  if (tag === 'SELECT') return el.multiple ? 'listbox' : 'combobox';
  if (tag === 'TEXTAREA') return 'textbox';
  if (tag === 'INPUT') {
   const type = (el.getAttribute('type') || 'text').toLowerCase();
   return type in INPUTS ? INPUTS[type] : 'textbox';
  }
  if (el.isContentEditable && !el.parentElement?.isContentEditable) return 'textbox';
  if (/^H[1-6]$/.test(tag)) return 'heading';
  return '';
 }

 function clickable(el) {
  if (el.hasAttribute('onclick')) return true;
  if (el.querySelector(SELECTOR)) return false;
  if (styleOf(el).cursor !== 'pointer') return false;
  const parent = el.parentElement;
  return !parent || styleOf(parent).cursor !== 'pointer';
 }

 function labelText(label, control) {
  let text = '';
  const walker = label.ownerDocument.createTreeWalker(label, NodeFilter.SHOW_TEXT);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) if (!control.contains(node)) text += ` ${node.data}`;
  return text;
 }

 function nameOf(el, role) {
  const by = el.getAttribute('aria-labelledby');
  let name = el.getAttribute('aria-label') || (by ? by.split(/\s+/).map(id => el.ownerDocument.getElementById(id)?.innerText || '').join(' ') : '');
  if (!name && el.labels?.length) name = [...el.labels].map(label => labelText(label, el)).join(' ');
  if (!name && TYPING.has(role)) name = el.getAttribute('placeholder') || el.getAttribute('title') || el.getAttribute('name') || '';
  if (!name && el.tagName === 'INPUT' && /^(button|submit|reset)$/i.test(el.type)) name = el.value;
  if (!name && !TYPING.has(role)) name = el.innerText;
  if (!name) name = el.getAttribute('title') || el.querySelector?.('img[alt]')?.alt || el.querySelector?.('svg title')?.textContent || '';
  return clean(name);
 }

 function idOf(el) {
  let id = state.ids.get(el);
  if (!id) {
   id = state.next++;
   state.ids.set(el, id);
   state.refs.set(id, new WeakRef(el));
  }
  return id;
 }

 function href(el) {
  const raw = el.getAttribute('href') || '';
  if (!raw || raw.startsWith('#') || /^javascript:/i.test(raw)) return '';
  try {
   const url = new URL(el.href);
   return cut(url.origin === location.origin ? `${url.pathname}${url.search}` : `${url.host}${url.pathname}`, 80);
  } catch {
   return '';
  }
 }

 function describe(el, role = roleOf(el) || (clickable(el) ? 'clickable' : el.tagName.toLowerCase())) {
  let text = role;
  const name = nameOf(el, role);
  if (name) text += ` ${quote(name)}`;
  if (TYPING.has(role) && el.tagName !== 'SELECT') {
   const value = el.isContentEditable ? el.innerText : el.value;
   if (el.type === 'password') { if (value) text += ' value=••••'; }
   else if (clean(value)) text += ` value=${quote(value, 160)}`;
   if (name !== clean(el.getAttribute('placeholder') || '') && el.getAttribute('placeholder') && !clean(value)) text += ` placeholder=${quote(el.getAttribute('placeholder'))}`;
  }
  if (el.tagName === 'SELECT') {
   const options = [...el.options].map(option => clean(option.text)).filter(Boolean);
   const chosen = el.selectedOptions[0];
   if (chosen) text += ` = ${quote(chosen.text)}`;
   text += ` options: ${options.slice(0, 15).join(' | ')}${options.length > 15 ? ` … ${options.length - 15} more` : ''}`;
  }
  if (/^(checkbox|radio|switch|menuitemcheckbox|menuitemradio)$/.test(role)) {
   const on = typeof el.checked === 'boolean' ? el.checked : el.getAttribute('aria-checked') === 'true';
   text += on ? ' checked' : ' unchecked';
  }
  if (el.getAttribute('aria-expanded')) text += el.getAttribute('aria-expanded') === 'true' ? ' expanded' : ' collapsed';
  if (el.getAttribute('aria-selected') === 'true' || el.getAttribute('aria-current') && el.getAttribute('aria-current') !== 'false') text += ' current';
  if (el.disabled || el.getAttribute('aria-disabled') === 'true') text += ' disabled';
  if (role === 'link') { const target = href(el); if (target) text += ` -> ${target}`; }
  if (el.ownerDocument.activeElement === el) text += ' focused';
  return text;
 }

 function ownText(el) {
  let text = '';
  for (const node of el.childNodes) if (node.nodeType === 3) text += node.data;
  return clean(text);
 }

 function snapshot({ full = false } = {}) {
  const vw = innerWidth, vh = innerHeight, max = full ? 40000 : 9000;
  const lines = [], refs = {};
  let size = 0, skipped = 0, visited = 0;
  const push = text => {
   if (size + text.length > max) { skipped++; return; }
   lines.push(text);
   size += text.length + 1;
  };
  const inView = r => r.width >= 1 && r.height >= 1 && (full || (r.bottom > 0 && r.top < vh && r.right > 0 && r.left < vw));
  const children = el => {
   if (el.shadowRoot) for (const child of el.shadowRoot.children) visit(child);
   for (const child of el.children) visit(child);
  };
  const visit = el => {
   if (++visited > 20000 || SKIP.has(el.tagName) || el.getAttribute('aria-hidden') === 'true' || el.hidden) return;
   const role = roleOf(el);
   if (ACTIVE.has(role) || (!role && clickable(el))) {
    if (inView(rectOf(el)) && shown(el)) {
     const id = idOf(el), text = describe(el, role || 'clickable');
     refs[id] = cut(text, 140);
     push(`[${id}] ${text}`);
    }
    if (role !== 'listbox') return;
   } else if (role === 'heading') {
    if (inView(rectOf(el)) && shown(el)) push(`heading${/^H[1-6]$/.test(el.tagName) ? ` ${el.tagName[1]}` : ''} ${quote(el.innerText, 160)}`);
    if (!el.querySelector(SELECTOR)) return;
   } else if (el.tagName === 'IMG') {
    const alt = clean(el.alt);
    if (alt && alt.length > 1 && inView(rectOf(el)) && shown(el)) push(`img ${quote(alt)}`);
    return;
   } else if (el.tagName === 'IFRAME') {
    let doc = null;
    try { doc = el.contentDocument; } catch {}
    if (doc?.body) children(doc.body);
    else if (inView(rectOf(el))) push(`iframe${el.title ? ` ${quote(el.title)}` : ''} (content not readable)`);
    return;
   } else {
    const own = ownText(el);
    const flat = (own || el.children.length) && [...el.children].every(child => INLINE.has(child.tagName));
    if (flat && !el.shadowRoot && !el.querySelector(SELECTOR)) {
     const text = clean(el.innerText);
     if (text && inView(rectOf(el)) && shown(el)) push(`text ${quote(text, 240)}`);
     return;
    }
    if (own.length > 1 && inView(rectOf(el)) && shown(el)) push(`text ${quote(own, 240)}`);
   }
   children(el);
  };
  children(document.body || document.documentElement);
  const scroller = document.scrollingElement || document.documentElement;
  const height = scroller.scrollHeight, top = scroller.scrollTop;
  return { lines, refs, skipped, truncated: skipped > 0 || visited > 20000, scroll: { top, height, vh, vw, below: Math.max(0, height - top - vh) } };
 }

 function element(ref) {
  const el = state.refs.get(Number(ref))?.deref();
  if (!el || !el.isConnected) throw Object.assign(new Error(`Element [${ref}] is no longer on the page. Take a new snapshot.`), { code: 'stale_ref' });
  return el;
 }

 function reveal(ref) {
  element(ref).scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'instant' });
  return true;
 }

 // Qt runJavaScript does not await promises. The host polls the same
 // mutation-quiet condition (300 ms, capped at 2 s) with cancellable timers.
 let lastMutation = Date.now();
 const observer = new MutationObserver(() => { lastMutation = Date.now(); });
 observer.observe(document, { subtree: true, childList: true, attributes: true, characterData: true });
 const quiet = () => Date.now() - lastMutation >= 300;
 window.__og?.dispose?.();
 function has(text) {
  const want = String(text).toLowerCase();
  if ((document.body?.innerText || '').toLowerCase().includes(want)) return true;
  const hosts = [...document.querySelectorAll('*')].filter(el => el.shadowRoot);
  for (let k = 0; k < hosts.length; k++) {
   const root = hosts[k].shadowRoot;
   for (const child of root.children) if ((child.innerText || child.textContent || '').toLowerCase().includes(want)) return true;
   hosts.push(...[...root.querySelectorAll('*')].filter(el => el.shadowRoot));
  }
  return false;
 }

 window.__og = { pageId, snapshot, reveal, quiet, has, dispose: () => observer.disconnect() };
}

const clean = text => text.replace(/[ \t\u00a0]+/g, ' ').replace(/ *\n */g, '\n').replace(/\n{3,}/g, '\n\n').trim();
const BLOCK = new Set(['P', 'DIV', 'SECTION', 'ARTICLE', 'MAIN', 'HEADER', 'FOOTER', 'ASIDE', 'NAV', 'UL', 'OL', 'TABLE', 'THEAD', 'TBODY', 'BLOCKQUOTE', 'FIGURE', 'FIGCAPTION', 'DL', 'DT', 'DD', 'FORM', 'FIELDSET', 'DETAILS', 'SUMMARY', 'HR', 'ADDRESS']);

// Highlighters often put each code line in its own element instead of a newline.
function code(node) {
 let out = '';
 const walk = el => {
  for (const child of el.childNodes) {
   if (child.nodeType === 3) { out += child.data; continue; }
   if (child.nodeType !== 1) continue;
   if (child.tagName === 'BR') { out += '\n'; continue; }
   walk(child);
   const line = /^(DIV|P|LI|TR)$/.test(child.tagName) || /(^|[\s_-])line(\s|$)/i.test(child.getAttribute('class') || '');
   if (line && !out.endsWith('\n')) out += '\n';
  }
 };
 walk(node);
 return out.replace(/\n+$/, '');
}

function flatten(node, base) {
 let out = '';
 for (const child of node.childNodes) {
  if (child.nodeType === 3) { out += child.data.replace(/\s+/g, ' '); continue; }
  if (child.nodeType !== 1) continue;
  const tag = child.tagName;
  if (tag === 'BR') out += '\n';
  else if (tag === 'PRE') out += `\n\n\`\`\`\n${code(child)}\n\`\`\`\n\n`;
  else if (/^H[1-6]$/.test(tag)) out += `\n\n${'#'.repeat(Number(tag[1]))} ${clean(flatten(child, base)).replace(/\n/g, ' ')}\n\n`;
  else if (tag === 'LI') out += `\n- ${clean(flatten(child, base))}\n`;
  else if (tag === 'TR') out += `\n${[...child.children].map(cell => clean(flatten(cell, base)).replace(/\n/g, ' ')).join(' | ')}`;
  else if (tag === 'A') {
   const inner = flatten(child, base), label = clean(inner);
   let href = '';
   try { href = new URL(child.getAttribute('href') || '', base).href; } catch {}
   out += label && /^https?:/.test(href) && label.length < 90 && !href.startsWith(`${base.split('#')[0]}#`) && label !== href ? `[${label}](${href})` : inner;
  } else if (tag === 'IMG') {
   const alt = (child.getAttribute('alt') || '').trim();
   if (alt) out += ` [image: ${alt}] `;
  } else {
   const inner = flatten(child, base);
   out += BLOCK.has(tag) ? `\n\n${inner}\n\n` : inner;
  }
 }
 return out;
}

function readable(html, url) {
 const doc = new DOMParser().parseFromString(html, 'text/html');
 for (const node of doc.querySelectorAll('script, style, noscript, template, svg, canvas, iframe, object, embed, link, meta, button, input, select, textarea')) node.remove();
 let root = doc.querySelector('article, main, [role="main"]') || doc.body;
 if (!root || clean(root.textContent).length < 200) root = doc.body;
 if (root === doc.body) for (const node of root.querySelectorAll('nav, footer, aside, [role="navigation"], [aria-hidden="true"]')) node.remove();
 const title = clean(doc.title || '');
 const body = root ? clean(flatten(root, url)) : '';
 return title ? `# ${title}\n\n${body}` : body;
}
