'use strict';

// The one way out of the app to a backend: an external process that speaks the Agent Backend Protocol (ABP v0, see
// docs/backend-interface.md), JSON-RPC 2.0 with one JSON object per line on its stdin and stdout. The host only relays.
// It reads no method, keeps none of the agent's state and never calls a model itself. With no backend configured it
// says so, and the page shows that in the chat instead of answering.
//
// A backend is configured by OPENGHOST_BACKEND, either a path to an executable or a JSON array of the executable and its
// arguments, or else by backend.json in the app's user data folder: { "command": ["/path/to/backend", "--abp"] }.
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

// A line longer than this is a broken backend, not a message: the host drops it rather than hold it in memory.
const MAX_LINE = 64 * 1024 * 1024;
// How long a backend has to exit by itself on quit before it is killed.
const GRACE = 2000;

function parseCommand(value) {
 if (Array.isArray(value)) {
  if (!value.length || !value.every(part => typeof part === 'string' && part)) throw new Error('A backend command is a list of strings');
  return { file: value[0], args: value.slice(1) };
 }
 if (typeof value !== 'string' || !value.trim()) return null;
 const text = value.trim();
 return text.startsWith('[') ? parseCommand(JSON.parse(text)) : { file: text, args: [] };
}

// Where the backend comes from: the environment first, then the user's backend.json. Null when none is configured.
function configured({ env = process.env, userData = '' } = {}) {
 if (env.OPENGHOST_BACKEND) return parseCommand(env.OPENGHOST_BACKEND);
 if (!userData) return null;
 let saved;
 try {
  saved = JSON.parse(fs.readFileSync(path.join(userData, 'backend.json'), 'utf8'));
 } catch (error) {
  if (error.code === 'ENOENT') return null;
  throw error;
 }
 return parseCommand(saved?.command);
}

const isMessage = value => !!value && typeof value === 'object' && !Array.isArray(value) && value.jsonrpc === '2.0';

class BackendHost {
 constructor({ command = null, cwd, onMessage = () => {}, onStatus = () => {}, log = console } = {}) {
  this.command = command;
  this.cwd = cwd;
  this.onMessage = onMessage;
  this.onStatus = onStatus;
  this.log = log;
  this.child = null;
  this.buffer = '';
  this.state = { state: command ? 'stopped' : 'none' };
 }

 get status() {
  return { ...this.state };
 }

 setState(state) {
  this.state = state;
  this.onStatus(this.status);
 }

 start() {
  if (!this.command || this.child) return this.status;
  let child;
  try {
   child = spawn(this.command.file, this.command.args, { cwd: this.cwd, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
  } catch (error) {
   this.setState({ state: 'error', error: error.message });
   return this.status;
  }
  this.child = child;
  this.buffer = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', chunk => this.receive(chunk));
  child.stderr.setEncoding('utf8');
  // A backend's logs go to its stderr, never into the protocol.
  child.stderr.on('data', chunk => this.log.error(`[backend] ${chunk.trimEnd()}`));
  child.stdin.on('error', () => {});
  child.once('spawn', () => { if (this.child === child) this.setState({ state: 'running', pid: child.pid }); });
  child.once('error', error => {
   if (this.child !== child) return;
   this.child = null;
   this.setState({ state: 'error', error: error.message });
  });
  child.once('exit', (code, signal) => {
   if (this.child !== child) return;
   this.child = null;
   this.setState({ state: 'exited', code, signal });
  });
  return this.status;
 }

 receive(chunk) {
  this.buffer += chunk;
  let at;
  while ((at = this.buffer.indexOf('\n')) >= 0) {
   const line = this.buffer.slice(0, at).trim();
   this.buffer = this.buffer.slice(at + 1);
   if (!line) continue;
   let message;
   try {
    message = JSON.parse(line);
   } catch {
    this.log.error(`[backend] not a JSON line: ${line.slice(0, 200)}`);
    continue;
   }
   if (isMessage(message)) this.onMessage(message);
   else this.log.error(`[backend] not a JSON-RPC 2.0 message: ${line.slice(0, 200)}`);
  }
  if (this.buffer.length > MAX_LINE) {
   this.log.error('[backend] a line went past the size limit and was dropped');
   this.buffer = '';
  }
 }

 // One message to the backend. False when there is no backend to take it or the message isn't JSON-RPC 2.0.
 send(message) {
  if (!this.child || !isMessage(message)) return false;
  this.child.stdin.write(`${JSON.stringify(message)}\n`);
  return true;
 }

 // Asks the backend to shut down, then ends it if it is still there after the grace period.
 stop(grace = GRACE) {
  const child = this.child;
  if (!child) return Promise.resolve();
  return new Promise(resolve => {
   const timer = setTimeout(() => child.kill('SIGKILL'), grace);
   child.once('exit', () => { clearTimeout(timer); resolve(); });
   this.send({ jsonrpc: '2.0', id: 'shutdown', method: 'shutdown' });
   child.stdin.end();
  });
 }
}

module.exports = { BackendHost, configured, parseCommand };
