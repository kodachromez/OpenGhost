'use strict';

// F21: configuration is trusted local execution, with literal argv and explicit source/portability rules.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { BackendHost, configured, parseCommand } = require('../desktop/backend-host');

function scratch(t) {
 const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'og-backend-config-'));
 t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
 return dir;
}
const save = (dir, command) => fs.writeFileSync(path.join(dir, 'backend.json'), JSON.stringify({ command }));

// Run the real startup function without booting Electron, including its error handling and cwd selection.
const main = fs.readFileSync(path.join(__dirname, '../desktop/main.js'), 'utf8');
const startup = main.match(/function startBackend\(\) \{[\s\S]*?\n\}/)[0];
function startFromMain({ env = {}, userData, home = os.homedir(), onMessage = () => {}, onStatus = () => {} }) {
 return vm.runInNewContext(`let backend;\n${startup}\nstartBackend(); backend;`, {
  app: { getPath: name => { assert.equal(name, 'userData'); return userData; } },
  os: { homedir: () => home }, BackendHost,
  configured: options => configured({ ...options, env }),
  toPage: (channel, value) => channel === 'backend:message' ? onMessage(value) : onStatus(value),
 });
}

const PROBE = `
 process.stdout.write(JSON.stringify({ jsonrpc: '2.0', method: 'probe', params: {
  cwd: process.cwd(), args: process.argv.slice(1), inherited: process.env.OPENGHOST_F21_INHERITED,
  path: process.env.PATH, nodeOptions: process.env.NODE_OPTIONS,
 } }) + '\\n');
 process.stdin.resume();
`;
async function probe(makeHost) {
 let backend, timer;
 try {
  const result = new Promise((resolve, reject) => {
   timer = setTimeout(() => reject(new Error('backend probe timed out')), 5000);
   backend = makeHost(message => resolve(message.params), status => {
    if (status.state === 'error') reject(new Error(status.error));
   });
   backend.start(); // Idempotent if main already started it.
  });
  return await result;
 } finally {
  clearTimeout(timer);
  await backend?.stop(250);
 }
}

test('plain commands are whole executable paths, not shell command lines', () => {
 for (const file of ['/opt/backend', '/opt/My Backend/backend', 'backend', './bin/backend', '../backend',
  '~/backend', '$HOME/backend', 'backend --flag', '"/opt/backend"', 'backend; echo nope', 'C:\\Tools\\backend.exe']) {
  assert.deepEqual(parseCommand(file), { file, args: [] });
  assert.deepEqual(parseCommand(` \t${file}\n`), { file, args: [] });
 }
});

test('array and JSON-array commands preserve empty args, whitespace, quotes and metacharacters', () => {
 for (const file of ['/opt/My Backend/backend', ' ./backend ', '[backend]', 'C:\\Program Files\\Backend\\backend.exe']) {
  const args = ['', '  ', '\t', '--abp', 'a b', '"quoted"', '$HOME', '~', '*.js', '$(echo nope)', ';', '|', '>', '\\'];
  const command = [file, ...args];
  assert.deepEqual(parseCommand(command), { file, args });
  assert.deepEqual(parseCommand(` \t${JSON.stringify(command)}\n`), { file, args });
 }
 assert.deepEqual(parseCommand('["backend"]'), { file: 'backend', args: [] });
});

test('invalid commands always throw, including blank executables, wrong types, malformed JSON and NUL', () => {
 for (const command of [undefined, null, true, 42, {}, [], '', ' \t\n', [''], [' \t'], [1], [null],
  ['backend', 1], ['backend', null], ['backend', {}], ['backend', []], 'back\0end', ['back\0end'], ['backend', 'arg\0'],
  '[', '["backend",]', '[1]', '[]', '["backend"] trailing']) {
  assert.throws(() => parseCommand(command), /command/, JSON.stringify(command));
 }
 // Validation must not silently skip sparse array entries either.
 assert.throws(() => parseCommand(['backend', , 'arg']), /arguments/);
});

test('unset/empty/whitespace env values fall back; absence of both sources means no backend', t => {
 const userData = scratch(t);
 for (const env of [{}, { OPENGHOST_BACKEND: '' }, { OPENGHOST_BACKEND: ' \t\n' }]) {
  assert.equal(configured({ env, userData }), null);
  assert.equal(configured({ env }), null);
 }
 for (const command of ['/bin/backend', ['/bin/backend', '--abp', ''], '["/bin/backend", "--abp"]']) {
  save(userData, command);
  for (const env of [{}, { OPENGHOST_BACKEND: '' }, { OPENGHOST_BACKEND: ' \t\n' }]) {
   assert.deepEqual(configured({ env, userData }), parseCommand(command));
  }
 }
});

test('nonblank env overrides the file; an invalid override errors without falling back', t => {
 const userData = scratch(t);
 save(userData, ['/bin/file-backend']);
 for (const command of ['/bin/env-backend', '["/bin/env-backend", "--abp", ""]']) {
  assert.deepEqual(configured({ env: { OPENGHOST_BACKEND: command }, userData }), parseCommand(command));
 }
 for (const command of ['[', '[]', '[1]', '["", "arg"]', '["backend", null]', 'bad\0path', null, false, 0]) {
  assert.throws(() => configured({ env: { OPENGHOST_BACKEND: command }, userData }), /^Error: OPENGHOST_BACKEND: /);
 }
 fs.writeFileSync(path.join(userData, 'backend.json'), 'invalid JSON');
 assert.deepEqual(configured({ env: { OPENGHOST_BACKEND: '/bin/env-backend' }, userData }), { file: '/bin/env-backend', args: [] });
 assert.throws(() => configured({ env: { OPENGHOST_BACKEND: '[' }, userData }), /^Error: OPENGHOST_BACKEND: /);
 assert.throws(() => configured({ env: { OPENGHOST_BACKEND: '  ' }, userData }), /^Error: backend\.json: /);
});

test('existing malformed files/commands are errors, never silently no backend', t => {
 const userData = scratch(t), file = path.join(userData, 'backend.json');
 for (const text of ['', '{', 'null', 'false', '1', '[]', '"backend"', '{}']) {
  fs.writeFileSync(file, text);
  assert.throws(() => configured({ env: {}, userData }), /^Error: backend\.json: /, text);
 }
 for (const command of [null, false, 1, {}, [], '', ' \t', [' '], ['backend', null], '[', 'bad\0path']) {
  save(userData, command);
  assert.throws(() => configured({ env: {}, userData }), /^Error: backend\.json: /, JSON.stringify(command));
 }
 fs.rmSync(file);
 fs.mkdirSync(file); // Unreadable as a config file even when tests run as root.
 assert.throws(() => configured({ env: {}, userData }), /^Error: backend\.json: /);
});

test('main preserves the actual configuration source in error status and does not spawn', t => {
 const userData = scratch(t);
 fs.writeFileSync(path.join(userData, 'backend.json'), '{');
 for (const [env, prefix] of [[{ OPENGHOST_BACKEND: '[' }, /^OPENGHOST_BACKEND: /], [{}, /^backend\.json: /]]) {
  const backend = startFromMain({ env, userData });
  assert.equal(backend.status.state, 'error');
  assert.match(backend.status.error, prefix);
  assert.equal(backend.command, null);
  assert.equal(backend.child, null);
 }
});

test('main spawns in home, inheriting app environment and passing literal argv without a shell', async t => {
 const userData = scratch(t);
 const keys = ['OPENGHOST_F21_INHERITED', 'NODE_OPTIONS'];
 const previous = keys.map(key => process.env[key]);
 process.env.OPENGHOST_F21_INHERITED = 'inherited unchanged';
 process.env.NODE_OPTIONS = '--no-warnings';
 try {
  const args = ['', ' \t ', '$OPENGHOST_F21_INHERITED', '$HOME', '~', '*', '$(echo expanded)', '; echo expanded', '|', '>', '"quoted"', '\\'];
  save(userData, [process.execPath, '-e', PROBE, '--', ...args]);
  const result = await probe((onMessage, onStatus) => startFromMain({ userData, onMessage, onStatus }));
  assert.equal(result.cwd, fs.realpathSync(os.homedir()));
  assert.notEqual(result.cwd, userData);
  assert.deepEqual(result.args, args);
  assert.equal(result.inherited, process.env.OPENGHOST_F21_INHERITED);
  assert.equal(result.path, process.env.PATH);
  assert.equal(result.nodeOptions, '--no-warnings');
 } finally {
  keys.forEach((key, i) => { if (previous[i] === undefined) delete process.env[key]; else process.env[key] = previous[i]; });
 }
});

test('standalone host without cwd inherits parent cwd', async () => {
 const result = await probe((onMessage, onStatus) => new BackendHost({
  command: parseCommand([process.execPath, '-e', PROBE]), onMessage, onStatus,
 }));
 assert.equal(result.cwd, fs.realpathSync(process.cwd()));
});

test('relative executable paths resolve from startup home, not backend.json; PATH names use inherited PATH', {
 skip: process.platform === 'win32' && 'POSIX symlinks; Windows path syntax is covered by parser tests',
}, async t => {
 const root = scratch(t), home = path.join(root, 'home with spaces'), userData = path.join(root, 'config');
 fs.mkdirSync(home); fs.mkdirSync(userData);
 fs.symlinkSync(process.execPath, path.join(home, 'backend with spaces'));
 save(userData, ['./backend with spaces', '-e', PROBE]);
 const relative = await probe((onMessage, onStatus) => startFromMain({ userData, home, onMessage, onStatus }));
 assert.equal(relative.cwd, fs.realpathSync(home));
 const previous = process.env.PATH;
 process.env.PATH = home;
 try {
  const bare = await probe((onMessage, onStatus) => new BackendHost({
   command: parseCommand(['backend with spaces', '-e', PROBE]), cwd: userData, onMessage, onStatus,
  }));
  assert.equal(bare.cwd, fs.realpathSync(userData));
  assert.equal(bare.path, home);
 } finally {
  if (previous === undefined) delete process.env.PATH; else process.env.PATH = previous;
 }
});
