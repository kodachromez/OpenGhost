// One strictly headless browser and one reused tab. No Electron/desktop bridge.
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const [manifest, output] = process.argv.slice(2);
const fixtures = JSON.parse(fs.readFileSync(manifest)).fixtures;
const root = path.resolve(import.meta.dirname, '../..');
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'openghost-parity-browser-'));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
fs.mkdirSync(output, {recursive: true});
const child = spawn(process.env.PARITY_BROWSER || '/opt/brave-bin/brave', [
  '--headless=new', '--ozone-platform=headless', '--remote-debugging-port=0',
  `--user-data-dir=${home}/profile`, '--no-first-run', '--no-default-browser-check',
  '--disable-background-networking', '--disable-component-update', '--disable-sync',
  '--disable-features=MediaRouter', '--password-store=basic', '--use-gl=angle',
  '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--force-device-scale-factor=1', 'about:blank'
], {env: {...process.env, HOME: home, DISPLAY: '', WAYLAND_DISPLAY: '',
          XDG_CONFIG_HOME: `${home}/config`, XDG_CACHE_HOME: `${home}/cache`}, stdio: ['ignore', 'ignore', 'pipe']});
let diagnostics = '';
child.stderr.on('data', data => diagnostics = (diagnostics + data).slice(-2*1024*1024));
let ws;
try {
  let port;
  for (let i=0; i<300; i++) {
    try { port = fs.readFileSync(`${home}/profile/DevToolsActivePort`, 'utf8').split('\n')[0]; break; } catch {}
    if (child.exitCode !== null) throw Error(`Headless browser exited: ${diagnostics}`);
    await sleep(50);
  }
  if (!port) throw Error(`No headless debugging port: ${diagnostics}`);
  const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  ws = new WebSocket(pages.find(p => p.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve,reject) => { ws.onopen=resolve; ws.onerror=reject; });
  let next=1, fixtureImage='', served=new Set(), mediaImages={}, mediaPending=new Set();
  const waiting = new Map(), errors = [];
  ws.onmessage = ({data}) => {
    const msg=JSON.parse(data);
    if (msg.id) { const call=waiting.get(msg.id); waiting.delete(msg.id); clearTimeout(call.timer); msg.error ? call.reject(Error(JSON.stringify(msg.error))) : call.resolve(msg.result); }
    if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails);
    if (msg.method === 'Fetch.requestPaused') {
      const p=msg.params;
      if(mediaPending.has(p.request.url)) return; // bounded fixture wait; navigation cancels
      const bytes=mediaImages[p.request.url] || (served.has(p.request.url) ? fixtureImage : '');
      if(bytes)
        void send('Fetch.fulfillRequest',{requestId:p.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'image/png'}],body:bytes});
      else void send('Fetch.failRequest',{requestId:p.requestId,errorReason:'BlockedByClient'});
    }
  };
  const send = (method,params={}) => new Promise((resolve,reject) => {
    const id=next++, timer=setTimeout(() => reject(Error(`CDP timeout: ${method}`)), 20000);
    waiting.set(id,{resolve,reject,timer}); ws.send(JSON.stringify({id,method,params}));
  });
  const evaluate = async expression => {
    const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
    if(r.exceptionDetails) throw Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  };
  await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable');
  // Fixtures are local-only. Neither model traffic nor remote image requests are allowed.
  await send('Fetch.enable',{patterns:[{urlPattern:'http://*'},{urlPattern:'https://*'}]});
  await send('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'},{name:'prefers-color-scheme',value:'dark'}]});
  await send('Page.addScriptToEvaluateOnNewDocument',{source:`
    localStorage.clear();
    window.openghost={desktop:true, platform:'linux', videoInfo: id =>
      window.__parityInfoPending ? new Promise(()=>{}) : Promise.resolve(window.__parityInfo?.[id] || null)};
    // Stable random colour order, not a replacement renderer.
    let seed=7; Math.random=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/4294967296);
  `});
  const adapter = fs.readFileSync(path.join(import.meta.dirname,'reference-fixture.js'),'utf8');
  const version = await send('Browser.getVersion');
  fs.writeFileSync(path.join(output,'environment.json'),JSON.stringify(version,null,2));
  for(let pass=0;pass<2;pass++) for(const f of fixtures) {
    if(f.manual) continue;
    fixtureImage=f.imageData||'';
    served=new Set(f.mediaUrls||['https://parity.invalid/image.png']);
    mediaImages=f.mediaImages||{}; mediaPending=new Set(f.mediaPending||[]);
    await send('Emulation.setDeviceMetricsOverride',{width:f.width||1280,height:f.height||840,deviceScaleFactor:f.dpr||1,mobile:false});
    await send('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:f.motion?'no-preference':'reduce'},{name:'prefers-color-scheme',value:f.systemDark===false?'light':'dark'}]});
    await send('Page.navigate',{url:pathToFileURL(path.join(root,'reference/openghost/index.html')).href});
    for(let i=0;i<200;i++) {
      if(await evaluate("document.readyState === 'complete' && typeof chat === 'object' && typeof chatList === 'object' && !!chatList")) break;
      if(i===199) throw Error('Reference did not load');
      await sleep(25);
    }
    if(f.motion && !f.splash) {
      await evaluate("document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));");
      await sleep(1100);
    }
    await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:(f.width||1280)-2,y:2});
    await evaluate(`window.__parityInfo=${JSON.stringify(f.videoInfo||{})}; window.__parityInfoPending=${!!f.videoInfoPending}; true`);
    await evaluate(adapter);
    await evaluate(`parityFixture(${JSON.stringify(f)})`);
    if(f.reference) await evaluate(f.reference+'; true');
    await sleep(f.wait||450);
    if(!f.keepFocus) await evaluate('document.activeElement?.blur(); true');
    await evaluate(`document.querySelector('.thread').scrollTop=${f.scroll||0};`);
    if(f.referenceAfter) await evaluate(f.referenceAfter+'; true');
    if(f.mediaIndex !== undefined) await evaluate(`{const el=document.querySelector('.md-gallery .media'); if(!el) throw Error('Missing media stack'); for(let i=0;i<${f.mediaIndex};i++) el.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight',bubbles:true}));} true`);
    if(f.mediaHover) {
      const selector=f.mediaHover==='mediaStack'?'.md-gallery .media':'.md-video';
      const at=await evaluate(`{const r=document.querySelector('${selector}').getBoundingClientRect(); [r.x+r.width/2,r.y+r.height/2]}`);
      await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:at[0],y:at[1]});
    }
    await sleep(f.afterWait||100);
    if(!f.skipImageCheck && f.imageData && (f.mediaUrls||['x']).length && !await evaluate("[...document.querySelectorAll('.md-gallery img, .md-video-img')].some(img=>img.complete && img.naturalWidth===240)"))
      throw Error('Synthetic gallery image did not load: '+f.id);
    const geometry = await evaluate(`Object.fromEntries(['.sidebar','.composer','.thread','.welcome'].map(s=>{const r=document.querySelector(s).getBoundingClientRect();return [s,[r.x,r.y,r.width,r.height]]}))`);
    geometry.media=await evaluate(`[...document.querySelectorAll('.md-media')].map(el=>{const r=el.getBoundingClientRect(); return [r.x,r.y,r.width,r.height];})`);
    geometry.mediaDetails=await evaluate(`[...document.querySelectorAll('.md-gallery-caption,.md-gallery-source,.md-gallery-lost,.link-chip,.md-video-title,.md-video-meta')].map(el=>{const r=el.getBoundingClientRect(),s=getComputedStyle(el); return {class:el.className,rect:[r.x,r.y,r.width,r.height],font:s.font,lineHeight:s.lineHeight};})`);
    if(f.referenceCheck && !await evaluate(f.referenceCheck)) throw Error('Media state assertion: '+f.id);
    const image=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
    const base=path.join(output,f.id+(pass?'.repeat':''));
    fs.writeFileSync(base+'.png',Buffer.from(image.data,'base64'));
    fs.writeFileSync(base+'.json',JSON.stringify(geometry,null,2));
    console.log(`captured ${pass} ${f.id}`);
  }
  fs.writeFileSync(path.join(output,'errors.json'),JSON.stringify(errors,null,2));
  if(errors.length) throw Error(`Reference exceptions: ${JSON.stringify(errors)}`);
} finally {
  ws?.close(); child.kill('SIGTERM');
  await Promise.race([new Promise(resolve=>child.once('exit',resolve)),sleep(3000)]);
  if(child.exitCode===null) { child.kill('SIGKILL'); await new Promise(resolve=>child.once('exit',resolve)); }
  fs.writeFileSync(path.join(output,'browser.log'),diagnostics);
  fs.rmSync(home,{recursive:true,force:true});
}
