'use strict';

// Focused real-DOM check, not the full backend/browser smoke suite.
// Run: node test/e2e/presentation-limits.cjs (isolated profile; no backend, credentials or network).
// If this Electron build cannot use headless Ozone, OPENGHOST_E2E_OZONE=x11 uses the local display with a hidden window.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const ROOT = path.resolve(__dirname, '../..');

if (!process.versions.electron) {
 const home = fs.mkdtempSync(path.join(os.tmpdir(), 'og-presentation-'));
 try {
  const electron = path.join(ROOT, 'node_modules/electron/dist', process.platform === 'win32' ? 'electron.exe' : 'electron');
  const result = spawnSync(electron, [__filename, `--ozone-platform=${process.env.OPENGHOST_E2E_OZONE || 'headless'}`, '--disable-gpu', '--force-prefers-reduced-motion'], {
   cwd: ROOT, timeout: 30000, encoding: 'utf8', maxBuffer: 1024 * 1024,
   env: { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, 'config'), XDG_CACHE_HOME: path.join(home, 'cache'), OPENGHOST_BACKEND: '', OPENGHOST_E2E_PROFILE: home },
  });
  assert.equal(result.status, 0, `${result.error || result.signal || ''}\n${result.stdout}\n${result.stderr}`);
  process.stdout.write(result.stdout);
 } finally { fs.rmSync(home, { recursive: true, force: true }); }
} else {
 const { app, BrowserWindow, session } = require('electron');
 if (!process.env.OPENGHOST_E2E_PROFILE) throw new Error('Run this test with node, not directly with Electron.');
 const profile = path.join(process.env.OPENGHOST_E2E_PROFILE, 'electron');
 fs.mkdirSync(profile, { recursive: true });
 app.setPath('userData', profile);
 app.setPath('sessionData', profile);
 const deadline = setTimeout(() => app.exit(1), 25000);
 app.whenReady().then(async () => {
  session.defaultSession.webRequest.onBeforeRequest((details, done) => done({ cancel: /^https?:/.test(details.url) }));
  const win = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await win.loadFile(path.join(ROOT, 'index.html'));
  const result = await win.webContents.executeJavaScript(`(${exercise.toString()})()`);
  console.log(result);
  clearTimeout(deadline);
  app.exit(0);
 }).catch(error => { console.error(error); clearTimeout(deadline); app.exit(1); });
}

async function exercise() {
 const check = (ok, message) => { if (!ok) throw new Error(message); };
 const requests = [], huge = '<backend label> '.repeat(4096);
 const providers = Array.from({ length: 40 }, (_, i) => ({
  id: `p${i}`, name: huge, group: huge, status: { connected: true }, methods: [
   { type: 'apiKey', label: huge, hint: huge, placeholder: huge, url: `https://example.invalid/${'x'.repeat(4096)}` },
   { type: 'oauth', label: huge, hint: huge, action: huge },
  ],
 }));
 const models = providers.map(item => ({ id: 'model', provider: item.id, name: `Model ${item.id}` }));
 Backend.state = 'ready';
 Backend.capabilities = { auth: { providers: true } };
 Backend.request = async (method, params) => {
  requests.push({ method, params });
  if (method === 'auth.providers') return providers;
  if (method === 'models.list') return models;
  if (method === 'auth.login') return { connected: true };
  throw new Error(`Unexpected request: ${method}`);
 };
 await settings.refreshAll();
 settings.open();
 settings.page('providers', true);
 const list = settings.list;
 check(list.querySelectorAll('.provider').length === 16, 'only 16 provider sections should be mounted');
 for (const node of list.querySelectorAll('.provider-name, .settings-label, [data-action="login"]')) check(node.textContent.length <= 256, 'bounded backend label');
 for (const node of list.querySelectorAll('.settings-hint span')) check(node.textContent.length <= 2048, 'bounded backend hint');
 check(!list.querySelector('a'), 'oversized destination must not become a truncated link');
 check(list.textContent.includes('URL is too long'), 'link omission is explicit');
 check(list.querySelector('.settings-key').placeholder.length <= 256, 'bounded placeholder after paint');
 list.querySelector('.settings-provider-next').click();
 check(list.querySelector('.provider').dataset.provider === 'p16', 'page 2 uses the original identities');
 check(document.activeElement.matches('.settings-provider-next'), 'keyboard focus follows the replacement pager');
 list.querySelector('.settings-provider-next').click();
 check(list.querySelectorAll('.provider').length === 8, 'final partial page');
 check(list.querySelector('.settings-provider-next').disabled, 'last-page Next is disabled');
 check(document.activeElement.matches('.settings-provider-prev'), 'focus moves to the enabled pager');
 settings.accounts.p39.querySelector('[data-action="login"]').click();
 await new Promise(resolve => setTimeout(resolve, 0));
 check(requests.some(item => item.method === 'auth.login' && item.params.provider === 'p39'), 'off-first-page action keeps its exact backend identity');
 check(settings.providers[0].name === huge, 'backend names remain intact');

 // Generalizing four fixed providers must not introduce P × M model scans.
 let reads = 0;
 for (const model of settings.models) {
  const provider = model.provider;
  Object.defineProperty(model, 'provider', { get() { reads++; return provider; } });
 }
 modelStage.build();
 check(reads < models.length * 8, 'model grouping should be linear in catalog size');
 check(modelStage.rows.length === 40 && modelStage.groups.length === 40, 'the model catalog is not truncated');
 check(modelStage.rows.at(-1).dataset.model === 'p39:model', 'last model remains selectable');
 check(modelStage.groups.every(node => node.textContent.length <= 256), 'bounded provider group labels');
 settings.dialog.close();

 await Usage.ready;
 for (const provider of providers) Usage.record({ provider: provider.id, model: 'model', input: 100, output: 20, requests: 1 });
 await Promise.resolve();
 const before = JSON.stringify(Usage.totals(0));
 const dialog = document.createElement('dialog'), root = document.createElement('section');
 dialog.append(root);
 document.body.append(dialog);
 dialog.showModal();
 const usage = new UsageSettings({ root, settings, dialog });
 usage.render(false);
 check(root.querySelectorAll('.usage-provider').length === 16, 'usage sections are paged');
 check(root.querySelectorAll('.usage-key').length === 17, 'chart legend is bounded with an Other group');
 check(root.querySelector('.usage-legend').textContent.includes('Other providers (24)'), 'chart discloses aggregation');
 for (const split of root.querySelectorAll('.usage-split')) {
  const sum = [...split.children].reduce((n, node) => n + Number(node.style.flexGrow), 0);
  check(split.children.length === 17 && sum === 4800, 'all provider shares survive the chart projection');
 }
 root.querySelector('.usage-provider-nav[data-step="1"]').click();
 check(root.querySelector('.usage-provider').dataset.provider === 'p16', 'real usage paging control works');
 check(document.activeElement.matches('.usage-provider-nav[data-step="1"]'), 'usage pager keeps focus');
 root.querySelector('.usage-provider-nav[data-step="1"]').click();
 check(root.querySelectorAll('.usage-provider').length === 8, 'last usage page');
 check(document.activeElement.matches('.usage-provider-nav[data-step="-1"]'), 'last usage page keeps an enabled focus target');
 check(JSON.stringify(Usage.totals(0)) === before, 'DOM bounds must not rewrite accounting');
 dialog.close();

 let args = { script: 'echo first; important-final-command' };
 for (let i = 0; i < 500; i++) args = { child: args };
 const card = new ApprovalCard(ApprovalCard.present(null, 'custom', args));
 document.body.append(card.el);
 check(card.el.querySelector('.approval-code').textContent === JSON.stringify(args), 'approval fallback remains complete and compact');
 card.allow.click();
 check(await card.answer === 'allow', 'approval decision wiring unchanged');
 return 'PASS: real Electron provider paging, label limits, catalog grouping, usage totals and compact approval fallback';
}
