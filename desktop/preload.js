'use strict';

const { contextBridge, ipcRenderer, webUtils } = require('electron');

contextBridge.exposeInMainWorld('openghost', {
 desktop: true,
 platform: process.platform,
 // Where a dropped or picked file lives on disk, so a backend on this computer can open it again later.
 pathOf: file => {
  try { return webUtils.getPathForFile(file) || ''; } catch { return ''; }
 },
 // A PDF's text, by its place on the disk or by its bytes; the viewer that reads it lives in the main process.
 readPdf: source => ipcRenderer.invoke('pdf:read', source),
 pickFolder: () => ipcRenderer.invoke('folder:pick'),
 revealFolder: folder => ipcRenderer.invoke('folder:reveal', folder),
 // Where chats started without a project folder keep their own folders, and letting go of one that stayed empty.
 chatsFolder: () => ipcRenderer.invoke('folder:chats'),
 releaseFolder: folder => ipcRenderer.invoke('folder:release', folder),
 setTitleBar: (color, symbols) => ipcRenderer.send('window:titlebar', color, symbols),
 setTheme: choice => ipcRenderer.invoke('theme:set', choice),
 store: {
  read: key => ipcRenderer.invoke('store:read', key),
  write: (key, value) => ipcRenderer.invoke('store:write', key, value),
  remove: key => ipcRenderer.invoke('store:remove', key),
 },
 browser: {
  onEvent: callback => ipcRenderer.on('browser:event', (event, data) => callback(data)),
  shown: value => ipcRenderer.send('browser:shown', value),
  // The browser's host tools, run on the panel's own webviews when the backend asks for them.
  run: (id, name, args) => ipcRenderer.invoke('browser:run', id, name, args),
  cancel: id => ipcRenderer.invoke('browser:cancel', id),
 },
 // A YouTube video's name and channel for its card, from YouTube's oEmbed.
 videoInfo: id => ipcRenderer.invoke('media:video-info', id),
 // The external backend: JSON-RPC 2.0 messages both ways (ABP v0, docs/backend-interface.md), and its process's state.
 // This is the only way the page reaches an agent; there is no model, key or tool code on this side.
 backend: {
  send: message => ipcRenderer.send('backend:send', message),
  onMessage: callback => ipcRenderer.on('backend:message', (event, message) => callback(message)),
  onStatus: callback => ipcRenderer.on('backend:status', (event, status) => callback(status)),
  status: () => ipcRenderer.invoke('backend:status'),
 },
});
