#!/usr/bin/env node
'use strict';

// TEST FIXTURE ONLY: a backend that starts descendants the way a real one runs its tools, one in its own process group
// and one in the backend's, reports their PIDs, then behaves as its argument says:
//   "graceful" exits on shutdown without ending them; "stubborn" ignores shutdown, stdin EOF and SIGTERM;
//   "crash" exits by itself straight away.
const { spawn } = require('node:child_process');
const readline = require('node:readline');

const mode = process.argv[2];
// `set -m` gives the background sleep a process group of its own; the shell then becomes a sleep in ours.
const tree = spawn('sh', ['-c', 'set -m; sleep 300 & echo $!; exec sleep 300'], { stdio: ['ignore', 'pipe', 'ignore'] });
const send = message => process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', ...message })}\n`);

tree.stdout.once('data', chunk => {
 send({ method: 'tree', params: { pids: [tree.pid, Number(String(chunk).trim())] } });
 if (mode === 'crash') setTimeout(() => process.exit(3), 50);
});

if (mode === 'stubborn') {
 process.on('SIGTERM', () => {});
 process.stdin.resume();
 setInterval(() => {}, 1000);
} else {
 readline.createInterface({ input: process.stdin }).on('line', line => {
  if (JSON.parse(line).method === 'shutdown') process.exit(0);
 });
}
