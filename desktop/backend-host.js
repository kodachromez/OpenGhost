'use strict';

// The one way out of the app to a backend: an external process that speaks the Agent Backend Protocol (ABP v0, see
// docs/backend-interface.md), JSON-RPC 2.0 with one JSON object per line on its stdin and stdout. The host only relays.
// It reads no method, keeps none of the agent's state and never calls a model itself. With no backend configured it
// says so, and the page shows that in the chat instead of answering.
//
// Trusted local code, not a sandboxed plugin. OPENGHOST_BACKEND or backend.json selects an executable, never a shell
// command line. See docs/backend-interface.md for command syntax, precedence, inherited environment/cwd and portability.
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { validate } = require('../backend-protocol');

// UTF-8 bytes per line, excluding LF (but including any whitespace/CR), in either direction.
const MAX_LINE = 64 * 1024 * 1024;
// Bound Node's stdin write queue, including framing; no separate host queue. Fits one maximum-sized message.
const MAX_PENDING = MAX_LINE + 1;
// How long a backend has to exit by itself on quit before it is killed.
const GRACE = 2000;
const POSIX = process.platform !== 'win32';

// Every process left in a session: on Linux read from /proc, so it includes the separate process groups a backend runs
// its tools in. Zombies are already dead and are skipped.
function sessionMembers(sid) {
 let names;
 try {
  names = fs.readdirSync('/proc');
 } catch {
  return [];
 }
 const members = [];
 for (const name of names) {
  if (!/^\d+$/.test(name)) continue;
  let stat;
  try {
   stat = fs.readFileSync(`/proc/${name}/stat`, 'utf8');
  } catch {
   continue;
  }
  // pid (comm) state ppid pgrp session ...; comm may hold spaces and parentheses.
  const [state, , , session] = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
  if (Number(session) === sid && state !== 'Z') members.push(Number(name));
 }
 return members;
}

const kill = (pid, signal = 'SIGKILL') => { try { process.kill(pid, signal); } catch {} };

// Ends whatever a backend left behind once it is gone, however it went. On POSIX the backend leads its own session, and
// the session outlives it while anything it started is still in it, so that ID cannot be reused while there is
// something to end. A descendant that started a session of its own (setsid) has left on purpose and is beyond this.
// Windows has no sessions to follow: there the tree is ended through taskkill before the backend itself is killed.
function reap(sid) {
 if (!POSIX || !sid) return;
 kill(-sid);
 // A process can fork while the last one is being ended, so look again until nothing is left.
 for (let pass = 0; pass < 20; pass++) {
  const members = sessionMembers(sid);
  if (!members.length) return;
  members.forEach(pid => kill(pid));
 }
}

function parseCommand(value) {
 if (typeof value === 'string') {
  const text = value.trim();
  if (text.startsWith('[')) {
   try { value = JSON.parse(text); }
   catch { throw new Error('command contains malformed JSON; expected an array of strings'); }
  } else value = [text];
 }
 if (!Array.isArray(value) || !value.length) throw new Error('command must be an executable string or a nonempty array of strings');
 const [file, ...args] = value;
 if (typeof file !== 'string' || !file.trim() || file.includes('\0')) throw new Error('command executable must be a nonblank string without NUL characters');
 if (args.some(arg => typeof arg !== 'string' || arg.includes('\0'))) throw new Error('command arguments must be strings without NUL characters (empty strings are allowed)');
 return { file, args };
}

// Blank/unset environment falls back to the file; invalid nonblank configuration is an error, never a fallback.
// Null means no environment override and no config file, not an invalid command.
function configured({ env = process.env, userData = '' } = {}) {
 const override = env.OPENGHOST_BACKEND;
 if (override !== undefined && !(typeof override === 'string' && !override.trim())) {
  try { return parseCommand(override); }
  catch (error) { throw new Error(`OPENGHOST_BACKEND: ${error.message}`); }
 }
 if (!userData) return null;
 try {
  const text = fs.readFileSync(path.join(userData, 'backend.json'), 'utf8');
  let saved;
  try { saved = JSON.parse(text); }
  catch { throw new Error('invalid JSON; expected an object with a command field'); }
  if (!saved || typeof saved !== 'object' || Array.isArray(saved)) throw new Error('expected an object with a command field');
  return parseCommand(saved.command);
 } catch (error) {
  if (error.code === 'ENOENT') return null;
  throw new Error(`backend.json: ${error.message}`);
 }
}

class BackendHost {
 constructor({ command = null, cwd, onMessage = () => {}, onStatus = () => {}, log = console } = {}) {
  this.command = command;
  this.cwd = cwd;
  this.onMessage = onMessage;
  this.onStatus = onStatus;
  this.log = log;
  this.child = null;
  this.buffer = Buffer.alloc(0);
  this.lineBytes = 0;
  this.discarding = false;
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
   // No shell or environment filtering: the configured backend runs with the app's user privileges and environment.
   child = spawn(this.command.file, this.command.args, { cwd: this.cwd, shell: false, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true, detached: POSIX });
  } catch (error) {
   this.setState({ state: 'error', error: error.message });
   return this.status;
  }
  this.child = child;
  this.buffer = Buffer.alloc(0);
  this.lineBytes = 0;
  this.discarding = false;
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
   reap(child.pid);
   if (this.child !== child) return;
   this.child = null;
   this.setState({ state: 'exited', code, signal });
  });
  return this.status;
 }

 receive(chunk) {
  if (!Buffer.isBuffer(chunk)) chunk = Buffer.from(chunk, 'utf8');
  let offset = 0;
  while (offset < chunk.length) {
   const newline = chunk.indexOf(10, offset);
   const end = newline < 0 ? chunk.length : newline;
   if (!this.discarding) {
    const length = this.lineBytes + end - offset;
    if (length > MAX_LINE) {
     this.buffer = Buffer.alloc(0);
     this.lineBytes = 0;
     this.discarding = true;
     this.log.error('[backend] a line went past the size limit and was dropped');
    } else {
     // Grow geometrically, not once per fragment; even one-byte chunks cannot build an unbounded fragment list.
     if (length > this.buffer.length) {
      const buffer = Buffer.allocUnsafe(Math.min(MAX_LINE, Math.max(length, this.buffer.length * 2, 4096)));
      this.buffer.copy(buffer, 0, 0, this.lineBytes);
      this.buffer = buffer;
     }
     chunk.copy(this.buffer, this.lineBytes, offset, end);
     this.lineBytes = length;
    }
   }
   offset = end + 1;
   if (newline < 0) break;
   if (this.discarding) { this.discarding = false; continue; }
   const line = this.buffer.toString('utf8', 0, this.lineBytes).trim();
   this.lineBytes = 0;
   if (!line) continue;
   let message;
   try {
    message = JSON.parse(line);
   } catch {
    this.log.error(`[backend] not a JSON line: ${line.slice(0, 200)}`);
    continue;
   }
   // The RPC peer must see malformed envelopes too, so it can reject a matching call or answer an invalid request.
   // This host only frames JSON; it never dispatches methods.
   this.onMessage(message);
  }
 }

 // False means rejected (unavailable, invalid, oversized or queue full); never retained for a later retry.
 send(message) {
  const stdin = this.child?.stdin;
  if (!stdin?.writable || stdin.writableLength >= MAX_PENDING || validate(message)) return false;
  const line = JSON.stringify(message);
  const bytes = Buffer.byteLength(line, 'utf8');
  if (bytes > MAX_LINE || stdin.writableLength + bytes + 1 > MAX_PENDING) return false;
  // write(false) still accepts the message. Node drains its own queue; our byte cap bounds it even if it never drains.
  stdin.write(`${line}\n`, 'utf8');
  return true;
 }

 // Asks the backend to shut down, then ends it if it is still there after the grace period.
 stop(grace = GRACE) {
  const child = this.child;
  if (!child) return Promise.resolve();
  return new Promise(resolve => {
   const timer = setTimeout(() => {
    if (POSIX) return child.kill('SIGKILL');
    // The tree is only known while the backend is alive, so taskkill goes first and ends the backend with it.
    const end = () => child.kill('SIGKILL');
    spawn('taskkill', ['/T', '/F', '/PID', String(child.pid)], { stdio: 'ignore', windowsHide: true }).once('error', end).once('exit', end);
   }, grace);
   child.once('exit', () => { clearTimeout(timer); resolve(); });
   this.send({ jsonrpc: '2.0', id: 'shutdown', method: 'shutdown' });
   child.stdin.end();
  });
 }
}

module.exports = { BackendHost, configured, parseCommand, sessionMembers };
