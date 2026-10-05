(() => {
'use strict';

// Tools the host runs for a backend: the built-in browser, on the panel's own webviews, where the user's logins live and
// where the user can take control and hand it back. They can't run anywhere else, so the frontend publishes them at the
// handshake (initialize → host.tools) and a backend calls them back with a host.tool request (docs/backend-interface.md).
// Targeting, deadlines and result metadata: docs/browser-host-tools.md.
// Whether a step needs the user's approval is the backend's call: it asks with approval.request before calling.
const PAGE_CHARS = 40000;

const INPUT = new Set(['browser_click', 'browser_type', 'browser_select', 'browser_press', 'browser_scroll']);
const fn = (name, description, properties, required = []) => ({ name, description, parameters: { type: 'object', properties: {
 tabId: { type: 'string', description: 'Stable tabId from a result or host.browser; otherwise the active tab is pinned at receipt.' },
 pageId: { type: 'string', description: 'Page identity from a snapshot. Required for input; a changed page fails without input. Re-snapshot after navigation.' },
 ...properties,
}, required: INPUT.has(name) ? [...required, 'pageId'] : required } });

const SCHEMAS = [
 fn('browser_navigate', 'Open a page in the built-in browser, the panel on the right of the app where the user\'s own logins live. Returns a snapshot of the screen: text, and the elements you can use, each with a [number].', {
  url: { type: 'string', description: 'An address (https://example.com or example.com), a local file path, words to search on Google, or back, forward, reload' },
 }, ['url']),
 fn('browser_snapshot', 'Describe what the page in the built-in browser shows right now: text, and the elements you can use with their [numbers]. Only what is on screen, unless full is true.', {
  full: { type: 'boolean', description: 'Cover the whole page, not only the visible part' },
 }),
 fn('browser_click', 'Click in the built-in browser like a person with a mouse: pass ref, the [number] from the latest snapshot, or x and y in page pixels read from a screenshot. Returns the new snapshot.', {
  ref: { type: 'integer', description: 'Element [number] from the latest snapshot' },
  x: { type: 'number', description: 'Horizontal position in page pixels, instead of ref' },
  y: { type: 'number', description: 'Vertical position in page pixels, instead of ref' },
  double: { type: 'boolean', description: 'Double click' },
 }),
 fn('browser_type', 'Type into a field in the built-in browser: clicks field ref, replaces its text and types like a keyboard. submit true presses Enter after. Without ref it types where the focus is.', {
  ref: { type: 'integer', description: 'Field [number] from the latest snapshot' },
  text: { type: 'string', description: 'What to type' },
  submit: { type: 'boolean', description: 'Press Enter after typing' },
  clear: { type: 'boolean', description: 'false keeps the text already in the field and adds to it' },
 }, ['text']),
 fn('browser_select', 'Choose an option of a dropdown list (a select element) in the built-in browser by its visible text or value. Menus built from other elements are opened with a click and chosen with a click.', {
  ref: { type: 'integer', description: 'The list [number] from the latest snapshot' },
  option: { type: 'string', description: 'Option text or value' },
 }, ['ref', 'option']),
 fn('browser_press', 'Press a key or a combination in the built-in browser: Enter, Escape, Tab, Backspace, Delete, Space, ArrowDown, PageDown, Home, End, Control+A, Shift+Tab and so on.', {
  key: { type: 'string', description: 'Key name or combination' },
  times: { type: 'integer', description: 'How many times, 1 by default' },
 }, ['key']),
 fn('browser_scroll', 'Scroll the page in the built-in browser down or up by a share of the screen, or bring element ref into view. Returns the new snapshot.', {
  direction: { type: 'string', enum: ['down', 'up'], description: 'down by default' },
  amount: { type: 'number', description: 'How many screens, 0.8 by default' },
  ref: { type: 'integer', description: 'Scroll this element into view instead' },
 }),
 fn('browser_screenshot', 'Look at the page in the built-in browser yourself: returns a picture of the screen, or of the page from the top with full_page. Use it when layout, images, colors, charts or the look of a site you build matter, or when the snapshot is not enough.', {
  full_page: { type: 'boolean', description: 'The page from the top, up to four screens tall' },
 }),
 fn('browser_read', 'Read a frozen HTML-derived text snapshot, with headings, lists and links. Not computed visibility: may include hidden text and omit form values, shadow roots and iframes. Long pages come in parts; continue with tabId, readId and start.', {
  start: { type: 'integer', description: 'UTF-16 code-unit offset (not bytes) in the frozen read snapshot' },
  readId: { type: 'string', description: 'Read identity from the first part; required for continuation. Start at 0 without readId to refresh.' },
 }),
 fn('browser_wait', 'Wait in the built-in browser until some text appears on the page, or for a number of seconds, then return the snapshot.', {
  text: { type: 'string', description: 'Text to wait for' },
  seconds: { type: 'number', description: 'How long to wait at most, 15 by default with text' },
 }),
 fn('browser_tabs', 'List tabs, open a new one (with url to load a page), switch or close using stable tabId from the list. Opening at the 12-tab limit fails without eviction. Browser calls are serialized across chats.', {
  action: { type: 'string', enum: ['list', 'new', 'switch', 'close'] },
  url: { type: 'string', description: 'For new: what to open in it' },
  tab: { type: 'integer', description: 'Display position only; use tabId for switch and close' },
 }, ['action']),
];

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

// browser_read gives the page's HTML; the backend gets its text, a part at a time.
function read(answer, start) {
 const text = readable(answer.html || '', answer.url || 'about:blank');
 const from = Math.max(0, Math.floor(Number(start) || 0)), part = text.slice(from, from + PAGE_CHARS), end = from + part.length;
 const tail = end < text.length ? `\n\n[Characters ${from}–${end} of ${text.length}. Call browser_read with tabId=${JSON.stringify(answer.tabId)}, readId=${JSON.stringify(answer.readId)}, start=${end} to read further.]` : '';
 return {
  text: `${answer.url || ''}\n\n${part || '(empty page)'}${tail}${answer.sourceTruncated ? '\n\n[Source HTML truncated at 4 Mi UTF-16 code units.]' : ''}`,
  start: from, end, total: text.length, hasMore: end < text.length, offsetUnit: 'utf16',
  sourceTruncated: !!answer.sourceTruncated, truncated: !!answer.sourceTruncated || end < text.length,
  coverage: 'html-derived; excludes form controls, shadow roots and iframe content; may include hidden text',
 };
}

// What the browser answered, as an ABP host.tool result: text and pictures, plus the snapshot's element names by their
// [number] (`data.refs`) for a backend that words its approval cards with them.
function result(args, answer) {
 const data = {};
 for (const key of ['code', 'tabId', 'pageId', 'readId', 'refs', 'tabs', 'truncated', 'coverage', 'scroll', 'width', 'height', 'scale', 'pageWidth', 'pageHeight', 'downloads']) {
  if (answer?.[key] !== undefined) data[key] = answer[key];
 }
 if (!answer || answer.error) return { isError: true, data: { ...data, code: answer?.code || 'browser_error' }, content: [{ type: 'text', text: `Error: ${answer?.error || 'the browser did not answer'}` }] };
 if (answer.image) return { content: [{ type: 'text', text: answer.text || '' }, { type: 'image', dataUrl: answer.image, label: 'Screenshot of the built-in browser' }], data };
 if (answer.html !== undefined) {
  const { text, ...pagination } = read(answer, args.start);
  return { content: [{ type: 'text', text }], data: { ...data, ...pagination } };
 }
 return { content: [{ type: 'text', text: answer.text || '' }], data };
}

window.HostTools = { get schemas() { return window.openghost?.browser?.run ? SCHEMAS : []; }, has: name => SCHEMAS.some(tool => tool.name === name), result };
})();
