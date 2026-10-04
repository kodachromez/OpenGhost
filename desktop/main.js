'use strict';

const { app, BrowserWindow, Menu, dialog, ipcMain, nativeTheme, net, shell } = require('electron');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Browser = require('./browser');
const Pdf = require('./pdf');
const { BackendHost, configured } = require('./backend-host');

const APP_ID = 'com.openghost.app';
const ROOT = path.join(__dirname, '..');
// Windows takes the .ico; macOS and Linux take the .png.
const ICON = path.join(__dirname, process.platform === 'win32' ? 'icon.ico' : 'icon.png');
const TITLE_BAR = { height: 36 };
// The theme picked in Settings → Appearance. The window is painted before the page loads,
// so these colors repeat --chat-bg and --titlebar-symbols from styles.css.
const THEMES = {
 choices: ['system', 'light', 'dark'],
 dark: { background: '#191919', symbols: '#9a9a9a' },
 light: { background: '#ffffff', symbols: '#5c5c5c' },
};
const STORE_KEY = /^[a-z0-9-]+(\/[a-z0-9-]+)?$/;
// A chat started without a project folder works in a folder of its own in here. It sits in the home folder, not next to the app:
// an update replaces the app's own folder whole, and Documents and the desktop are often synced by OneDrive or iCloud.
const CHATS = path.join(os.homedir(), 'OpenGhost', 'Chats');
const YOUTUBE_ID = /^[\w-]{11}$/;

process.env.ELECTRON_DISABLE_SECURITY_WARNINGS = 'true';
app.setAppUserModelId(APP_ID);
// Windows and Linux show no menu bar: copying, pasting and undo work in the page by themselves. macOS delivers Cmd+C, Cmd+V,
// Cmd+A, Cmd+Z, Cmd+Q, Cmd+H and Cmd+M only through the menu at the top of the screen, so there the app keeps the standard
// app, edit and window menus.
Menu.setApplicationMenu(process.platform === 'darwin' ? Menu.buildFromTemplate([{ role: 'appMenu' }, { role: 'editMenu' }, { role: 'windowMenu' }]) : null);

const themeFile = () => path.join(app.getPath('userData'), 'theme.json');

function readTheme() {
 try {
  const { choice } = JSON.parse(fs.readFileSync(themeFile(), 'utf8'));
  if (THEMES.choices.includes(choice)) return choice;
 } catch {}
 return 'dark';
}

// 'system' lets the page and the sites in the built-in browser follow the computer; light and dark hold them to one look.
nativeTheme.themeSource = readTheme();
const look = () => THEMES[nativeTheme.shouldUseDarkColors ? 'dark' : 'light'];

function createShortcut() {
 const link = path.join(app.getPath('desktop'), 'OpenGhost.lnk');
 const ok = shell.writeShortcutLink(link, 'create', {
  target: process.execPath,
  args: `"${ROOT}"`,
  cwd: ROOT,
  icon: ICON,
  iconIndex: 0,
  appUserModelId: APP_ID,
  description: 'OpenGhost',
 });
 console.log(ok ? `Shortcut: ${link}` : 'Could not create the shortcut');
}

const storeDir = () => path.join(app.getPath('userData'), 'store');
const writes = new Map();

function storeFile(key) {
 if (typeof key !== 'string' || !STORE_KEY.test(key)) throw new Error(`Bad store key: ${key}`);
 return path.join(storeDir(), `${key}.json`);
}

async function readStore(key) {
 try {
  return JSON.parse(await fs.promises.readFile(storeFile(key), 'utf8'));
 } catch (error) {
  if (error.code === 'ENOENT') return null;
  throw error;
 }
}

function writeStore(key, value) {
 const file = storeFile(key), data = JSON.stringify(value);
 const next = (writes.get(file) || Promise.resolve()).catch(() => {}).then(async () => {
  await fs.promises.mkdir(path.dirname(file), { recursive: true });
  const temp = `${file}.tmp`;
  await fs.promises.writeFile(temp, data, 'utf8');
  await fs.promises.rename(temp, file);
 });
 writes.set(file, next);
 next.finally(() => { if (writes.get(file) === next) writes.delete(file); }).catch(() => {});
 return next;
}

async function removeStore(key) {
 const file = storeFile(key);
 await (writes.get(file) || Promise.resolve()).catch(() => {});
 await fs.promises.rm(file, { force: true });
}

const inChats = dir => {
 const rest = path.relative(CHATS, dir);
 return !!rest && !rest.startsWith('..') && !path.isAbsolute(rest);
};

// A deleted chat takes its own folder along only while nothing is in it: what was made there stays the user's.
async function release(dir) {
 if (typeof dir !== 'string' || !path.isAbsolute(dir) || !inChats(dir)) return false;
 return fs.promises.rmdir(dir).then(() => true, () => false);
}

// The browser's host tools: the backend asks the page for them (host.tool), the page runs them here on its own webviews.
const browserJobs = new Map();

async function runBrowser(id, name, args, sender) {
 if (typeof name !== 'string' || !name.startsWith('browser_')) return { error: `Unknown browser tool ${name}` };
 const controller = new AbortController(), key = String(id || '');
 if (key) browserJobs.set(key, () => controller.abort());
 try {
  return await Browser.run(name, args && typeof args === 'object' ? args : {}, sender, controller.signal);
 } catch (error) {
  // `stopped`: the step was cancelled and ended before its next action, so the page may show only part of it.
  return { error: error.message, code: error.code || 'browser_error', stopped: controller.signal.aborted };
 } finally {
  browserJobs.delete(key);
 }
}

const cancelBrowser = () => { for (const stop of browserJobs.values()) stop(); };

// What YouTube tells about a video for its card: its name and who made it. Only YouTube's oEmbed is ever asked.
async function videoInfo(id) {
 if (typeof id !== 'string' || !YOUTUBE_ID.test(id)) return null;
 const address = `https://www.youtube.com/oembed?url=${encodeURIComponent(`https://www.youtube.com/watch?v=${id}`)}&format=json`;
 try {
  const response = await net.fetch(address, { signal: AbortSignal.timeout(10000) });
  if (!response.ok) return null;
  const data = await response.json();
  return { title: String(data.title || ''), by: String(data.author_name || '') };
 } catch {
  return null;
 }
}

function external(url) {
 if (/^(https?|mailto):/i.test(url)) shell.openExternal(url);
}

function createWindow() {
 const win = new BrowserWindow({
  width: 1280,
  height: 840,
  minWidth: 760,
  minHeight: 540,
  show: false,
  title: 'OpenGhost',
  icon: ICON,
  backgroundColor: look().background,
  // Linux window managers draw their own title bar; Windows and macOS get the app's own.
  ...(process.platform === 'linux' ? {} : {
   titleBarStyle: 'hidden',
   titleBarOverlay: { color: look().background, symbolColor: look().symbols, height: TITLE_BAR.height },
  }),
  webPreferences: {
   preload: path.join(__dirname, 'preload.js'),
   contextIsolation: true,
   sandbox: true,
   spellcheck: true,
   webviewTag: true,
  },
 });
 win.once('ready-to-show', () => win.show());
 win.webContents.on('will-attach-webview', (event, prefs, params) => {
  if (!Browser.guard(win.webContents, prefs, params)) event.preventDefault();
 });
 win.webContents.on('did-attach-webview', (event, guest) => Browser.adopt(win.webContents, guest));
 win.webContents.setWindowOpenHandler(({ url }) => {
  external(url);
  return { action: 'deny' };
 });
 win.webContents.on('will-navigate', (event, url) => {
  if (url === win.webContents.getURL()) return;
  event.preventDefault();
  external(url);
 });
 win.webContents.on('before-input-event', (event, input) => {
  if (input.type !== 'keyDown') return;
  const key = input.key.toLowerCase();
  // Cmd on macOS, Ctrl elsewhere.
  const command = input.meta || input.control;
  if (key === 'f12' || (command && input.shift && key === 'i') || (input.meta && input.alt && key === 'i')) {
   win.webContents.toggleDevTools();
   event.preventDefault();
  } else if (key === 'f5' || (command && !input.shift && key === 'r')) {
   win.webContents.reload();
   event.preventDefault();
  }
 });
 win.loadFile(path.join(ROOT, 'index.html'));
 return win;
}

ipcMain.handle('folder:pick', async event => {
 const win = BrowserWindow.fromWebContents(event.sender);
 const result = await dialog.showOpenDialog(win, { properties: ['openDirectory', 'createDirectory', 'promptToCreate'] });
 if (result.canceled || !result.filePaths.length) return null;
 const folder = result.filePaths[0];
 await fs.promises.mkdir(folder, { recursive: true });
 return { path: folder, name: path.basename(folder) || folder };
});

ipcMain.handle('folder:reveal', (event, folder) => typeof folder === 'string' && shell.openPath(folder));
ipcMain.handle('folder:chats', () => CHATS);
ipcMain.handle('folder:release', (event, folder) => fromApp(event) && release(folder));
ipcMain.handle('store:read', (event, key) => readStore(key));
ipcMain.handle('store:write', (event, key, value) => writeStore(key, value));
ipcMain.handle('store:remove', (event, key) => removeStore(key));
// The page sends its background once it opens and whenever the theme changes: the title bar buttons sit on it,
// and the window shows it wherever the page has not painted yet, as while resizing.
ipcMain.on('window:titlebar', (event, color, symbols) => {
 const win = BrowserWindow.fromWebContents(event.sender);
 const hex = value => typeof value === 'string' && /^#[0-9a-f]{6}$/i.test(value);
 if (!win || !hex(color)) return;
 win.setBackgroundColor(color);
 if (process.platform === 'linux' || typeof win.setTitleBarOverlay !== 'function') return;
 win.setTitleBarOverlay({ color, symbolColor: hex(symbols) ? symbols : look().symbols, height: TITLE_BAR.height });
});

const fromApp = event => event.sender.getType() === 'window' && event.senderFrame?.url.startsWith('file:');

// Answers whether the app ends up dark: for 'system' only this side knows what the computer uses right now.
ipcMain.handle('theme:set', (event, choice) => {
 if (fromApp(event) && THEMES.choices.includes(choice) && nativeTheme.themeSource !== choice) {
  nativeTheme.themeSource = choice;
  fs.promises.writeFile(themeFile(), JSON.stringify({ choice })).catch(() => {});
 }
 return nativeTheme.shouldUseDarkColors;
});
ipcMain.handle('browser:run', (event, id, name, args) => fromApp(event) ? runBrowser(id, name, args, event.sender) : { error: 'Not allowed' });
ipcMain.handle('browser:cancel', (event, id) => { if (fromApp(event)) browserJobs.get(String(id))?.(); });
ipcMain.on('browser:shown', (event, value) => { if (fromApp(event)) Browser.setShown(value); });
ipcMain.handle('pdf:read', (event, source) => fromApp(event) ? Pdf.read(source) : { text: '', reason: 'unreadable' });
ipcMain.handle('media:video-info', (event, id) => fromApp(event) ? videoInfo(id) : null);

// The backend: the page sends and gets JSON-RPC messages through here, and hears when the backend comes or goes.
let backend = null;
let page = null;
const toPage = (channel, data) => { if (page && !page.isDestroyed()) page.send(channel, data); };
ipcMain.on('backend:send', (event, message) => { if (fromApp(event)) backend?.send(message); });
ipcMain.handle('backend:status', event => fromApp(event) && backend ? backend.status : { state: 'none' });

function startBackend() {
 let command = null, error = '';
 try {
  command = configured({ userData: app.getPath('userData') });
 } catch (e) {
  error = e.message;
 }
 backend = new BackendHost({
  command,
  cwd: os.homedir(),
  onMessage: message => toPage('backend:message', message),
  onStatus: status => toPage('backend:status', status),
 });
 if (error) backend.setState({ state: 'error', error });
 else backend.start();
}

if (process.argv.includes('--create-shortcut')) {
 app.whenReady().then(() => {
  createShortcut();
  app.quit();
 });
} else if (!app.requestSingleInstanceLock()) {
 app.quit();
} else {
 let win = null;
 app.on('second-instance', () => {
  if (!win) return;
  if (win.isMinimized()) win.restore();
  win.focus();
 });
 app.whenReady().then(() => {
  Browser.setup();
  startBackend();
  win = createWindow();
  page = win.webContents;
  win.on('closed', () => {
   win = null;
   page = null;
   cancelBrowser();
   Pdf.cancelAll();
  });
 });
 app.on('window-all-closed', () => app.quit());
 app.on('before-quit', event => {
  cancelBrowser();
  if (!writes.size && !backend?.child) return;
  event.preventDefault();
  // The store is on disk and the backend has had its chance to shut down before the app goes.
  Promise.allSettled([...writes.values()]).then(() => backend?.stop()).then(() => app.quit());
 });
}
