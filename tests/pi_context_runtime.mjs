// Optional offline qualification against an installed Pi SDK (not a native dependency):
// PI_SDK_ROOT=/path/to/@earendil-works/pi-coding-agent node tests/pi_context_runtime.mjs
// Real Pi sessions/compaction/bridge; faux model only. No credentials or network.
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';

assert.ok(process.env.PI_SDK_ROOT, 'Set PI_SDK_ROOT to the installed Pi package');
const root = resolve(process.env.PI_SDK_ROOT);
const sdk = await import(pathToFileURL(join(root, 'dist/index.js')));
const ai = await import(pathToFileURL(join(root, '../pi-ai/dist/index.js')));
// Keep the production hooks/command intact; auth is outside this qualification.
const source = (await readFile(new URL('../src/backend/pi/openghost-bridge.js', import.meta.url), 'utf8'))
  .replace('import { SettingsManager } from "@earendil-works/pi-coding-agent";', 'const SettingsManager = {};');
const { default: bridge } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
const dir = await mkdtemp(join(tmpdir(), 'og-context-audit-'));
const faux = ai.fauxProvider({ provider: 'og-audit', tokensPerSecond: Infinity });
const requests = [];
const response = (text, options) => (context) => {
  requests.push(structuredClone(context.messages));
  return ai.fauxAssistantMessage(text, options);
};
let session;
try {
  const runtime = await sdk.ModelRuntime.create({ authPath: join(dir, 'auth.json'),
    modelsPath: null, modelsStorePath: join(dir, 'models-cache.json'), refreshOnCreate: false });
  runtime.registerNativeProvider(faux.provider);
  const settings = sdk.SettingsManager.inMemory({ compaction: { enabled: false, keepRecentTokens: 0 },
    retry: { enabled: false } });
  const create = async (manager) => {
    const loader = new sdk.DefaultResourceLoader({ cwd: dir, agentDir: dir, settingsManager: settings,
      noExtensions: true, noSkills: true, noPromptTemplates: true, noThemes: true, noContextFiles: true,
      extensionFactories: [bridge] });
    await loader.reload();
    assert.deepEqual(loader.getExtensions().errors, []);
    const result = await sdk.createAgentSession({ cwd: dir, agentDir: dir, resourceLoader: loader,
      modelRuntime: runtime, model: faux.getModel(), tools: [], settingsManager: settings, sessionManager: manager });
    await result.session.bindExtensions({});
    return result.session;
  };
  session = await create(sdk.SessionManager.create(dir, dir));
  const command = (op) => session.prompt('/openghost ' + JSON.stringify({ token: 'audit', ...op }));
  const setContext = (instructions) => command({ op: 'context', instructions,
    files: instructions ? [{ name: 'notes.txt', text: 'PINNED-NOTE' }] : [] });
  const check = (text) => {
    const messages = requests.at(-1);
    const prompt = ai.getCurrentSystemPrompt(messages);
    assert.equal(prompt.includes('STANDING-A'), text === 'STANDING-A', prompt);
    assert.equal(prompt.includes('STANDING-B'), text === 'STANDING-B', prompt);
    assert.equal(prompt.includes('PINNED-NOTE'), !!text, prompt);
    assert.ok(messages.filter((m) => m.role !== 'system').every((m) =>
      !JSON.stringify(m).includes('STANDING-') && !JSON.stringify(m).includes('PINNED-NOTE')));
  };
  const mark = (turn) => command({ op: 'mark', entry: { event: 'start', turn } });
  const retry = async (failedTurnId) => {
    const settled = new Promise((resolve, reject) => {
      const timer = setTimeout(() => { off(); reject(new Error('Retry did not settle')); }, 5000);
      const off = session.subscribe((event) => {
        if (event.type === 'agent_settled') { clearTimeout(timer); off(); resolve(); }
      });
    });
    await command({ op: 'retry', failedTurnId });
    await settled;
  };
  await setContext('STANDING-A');
  faux.setResponses([response('ordinary')]);
  await session.prompt('hello');
  check('STANDING-A');
  faux.setResponses([response('offline summary')]);
  await session.compact();
  assert.ok(session.sessionManager.getEntries().some((e) => e.type === 'compaction'));
  faux.setResponses([response('after compaction')]);
  await session.prompt('continue');
  check('STANDING-A');

  await mark('failure');
  faux.setResponses([response('', { stopReason: 'error', errorMessage: 'fixture failure' })]);
  await session.prompt('fail');
  check('STANDING-A');
  // Pi's Retry continuation has no before_agent_start. It must still receive
  // current standing context, on every model request, including a tool loop.
  await setContext('STANDING-B');
  await mark('retry');
  faux.setResponses([response(ai.fauxToolCall('unregistered_fixture_tool', {}), { stopReason: 'toolUse' }),
                     response('recovered')]);
  await retry('failure');
  check('STANDING-B');
  assert.ok(ai.getCurrentSystemPrompt(requests.at(-2)).includes('STANDING-B'));

  await mark('failure-2');
  faux.setResponses([response('', { stopReason: 'error', errorMessage: 'fixture failure' })]);
  await session.prompt('fail again');
  const file = session.sessionFile;
  session.dispose();
  session = await create(sdk.SessionManager.open(file));
  await setContext('STANDING-A');
  await mark('retry-2');
  faux.setResponses([response('recovered after restart')]);
  await retry('failure-2');
  check('STANDING-A');
  await setContext('');
  faux.setResponses([response('cleared')]);
  await session.prompt('plain');
  check('');
  console.log('Pi context runtime: prompt, compaction, Retry/tool loop, restart and clearing passed.');
} finally {
  session?.dispose();
  await rm(dir, { recursive: true, force: true });
}
