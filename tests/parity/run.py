#!/usr/bin/env python3
"""Desktop-safe full parity audit. Exit 1 for capture failures, 2 for --strict differences."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import contextlib
import select
import signal
import csv
import hashlib
from html import escape
from triage import classify, MANUAL
from metrics import measure
from collections import Counter

# Allow finally blocks to reap the private display and owned renderer group.
def terminated(signum, frame):
    raise SystemExit(128 + signum)
signal.signal(signal.SIGTERM, terminated)
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, default=ROOT/'build-release/openghost-cpp')
parser.add_argument('--output', type=Path, default=ROOT/'build-release/visual-parity')
parser.add_argument('--manifest', type=Path, help='Optional custom manifest (default: all shared fixtures)')
parser.add_argument('--media', action='store_true', help='All reply-media fixtures plus sent-image boundary; not the unrelated suite')
parser.add_argument('--strict', action='store_true', help='Fail unless every comparable pixel is exact')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
if args.manifest is None:
    from fixtures import fixtures as make_fixtures
    args.manifest = out/'input.json'
    selected=make_fixtures()
    if args.media:
        selected=[f for f in selected if f.get('replyMedia') or f['id']=='attachment-image']
    args.manifest.write_text(json.dumps(dict(fixtures=selected),indent=2)+'\n')
fixtures = json.loads(args.manifest.read_text())['fixtures']
# Pin what was actually audited; no historical image baseline is consumed.
subprocess.run(['git','diff','--exit-code','HEAD','--','reference/openghost'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
def tree_hash(paths):
    digest=hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode()+b'\0'+path.read_bytes())
    return digest.hexdigest()
source_paths=[p for folder in ('src','qml','shaders','tests/parity') for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in str(p)]
metadata=dict(commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              reference_sha256=tree_hash(p for p in (ROOT/'reference/openghost').rglob('*') if p.is_file()),
              native_source_sha256=tree_hash(source_paths),
              binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              exact_threshold=0, diagnostic_channel_threshold=8,
              fonts=['Noto Sans','Noto Sans Mono'], font_files=[subprocess.check_output(['fc-match','-f','%{file}',name],text=True) for name in ['Noto Sans','Noto Sans Mono']],
              dimensions='1280x840 default; 760x540, 900x640, 1600x1000; DPR 1 and 2',
              qt_platform='offscreen on private Xvfb GLX, LIBGL_ALWAYS_SOFTWARE=1',
              browser='headless Chromium/SwiftShader; no external HTTP(S) requests permitted')
metadata['font_sha256']={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in metadata['font_files']}
(out/'environment.json').write_text(json.dumps(metadata,indent=2)+'\n')
assert len({f['id'] for f in fixtures}) == len(fixtures)
for f in fixtures:
    assert f['id'] and all(c.isalnum() or c == '-' for c in f['id'])

@contextlib.contextmanager
def private_display(home):
    # Qt's offscreen plugin here needs GLX for its OpenGL context. Xvfb is a
    # private memory-only display, never the user's X/Wayland server. No WM.
    executable = os.environ.get('PARITY_XVFB') or shutil.which('Xvfb')
    if not executable:
        local = ROOT/'build-release/parity-tools/usr/bin/Xvfb'
        if local.exists(): executable = str(local)
    if not executable: raise RuntimeError('Xvfb required for offscreen GLX; no visible fallback')
    read_fd, write_fd = os.pipe()
    with (out/'xvfb.log').open('w') as log:
        child = subprocess.Popen([executable, '-displayfd', str(write_fd), '-screen', '0',
                                  '2048x1400x24', '-nolisten', 'tcp', '-noreset'],
                                 pass_fds=(write_fd,), stdout=log, stderr=log,
                                 env={**os.environ, 'HOME':home, 'DISPLAY':'', 'WAYLAND_DISPLAY':''})
        os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 10)[0]: raise RuntimeError('Private Xvfb did not start')
            number = os.read(read_fd, 64).decode().strip()
            if not number.isdigit(): raise RuntimeError('Private Xvfb failed')
            yield ':'+number
        finally:
            os.close(read_fd)
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()

with tempfile.TemporaryDirectory(prefix='openghost-parity-') as home, private_display(home) as display:
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home+'/config', XDG_DATA_HOME=home+'/data',
               XDG_CACHE_HOME=home+'/cache', QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='generic',
               QT_OPENGL='software', QSG_RHI_BACKEND='opengl',
               QT_QUICK_BACKEND='rhi', LIBGL_ALWAYS_SOFTWARE='1', EGL_PLATFORM='surfaceless', QSG_INFO='1',
               QT_SCALE_FACTOR='1', QT_FONT_DPI='96', QT_FORCE_STDERR_LOGGING='1',
               OPENGHOST_REDUCED_MOTION='1', LC_ALL='C.UTF-8', TZ='UTC',
               XDG_RUNTIME_DIR=home+'/runtime', DBUS_SESSION_BUS_ADDRESS='')
    Path(env['XDG_RUNTIME_DIR']).mkdir(mode=0o700)
    for key in ('DISPLAY','WAYLAND_DISPLAY','QT_PLUGIN_PATH','QT_STYLE_OVERRIDE','QT_QPA_OFFSCREEN_NO_GLX'):
        env.pop(key, None)
    env['DISPLAY'] = display
    commands = [(['node', str(Path(__file__).with_name('reference.mjs')), str(args.manifest.resolve()), str(out/'reference')], 'reference.log', env)]
    for dpr in sorted({f.get('dpr',1) for f in fixtures if not f.get('manual')}):
        group = out/f'input-{dpr}x.json'
        group.write_text(json.dumps(dict(fixtures=[f for f in fixtures if f.get('dpr',1)==dpr and not f.get('manual')]),indent=2)+'\n')
        commands.append(([str(args.binary.resolve()), '--parity-manifest', str(group), '--parity-output', str(out/'native')],
                         f'native-{dpr}x.log', dict(env, QT_SCALE_FACTOR=str(dpr))))
    for command, log, command_env in commands:
        print('Running', ' '.join(command), flush=True)
        with (out/log).open('w') as stream:
            child = subprocess.Popen(command, cwd=ROOT, env=command_env, stdout=stream,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=max(120, len(fixtures)*12))
                if code: raise subprocess.CalledProcessError(code, command)
            except BaseException:
                # Node owns a browser process tree. A timeout/interrupt must
                # not leave that tree running after killing just its driver.
                try: os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                raise

results=[]
(out/'diff').mkdir(exist_ok=True)
for f in fixtures:
    category, notes = classify(f)
    if f.get('manual'):
        results.append(dict(id=f['id'],status='manual',reason=f['reason'],classification=category))
        continue
    images={}
    for side in ('reference','native'):
        images[side]=np.asarray(Image.open(out/side/(f['id']+'.png')).convert('RGB'))
        images[side+'Repeat']=np.asarray(Image.open(out/side/(f['id']+'.repeat.png')).convert('RGB'))
    a,b=images['reference'],images['native']
    expected=(f.get('height',840)*f.get('dpr',1),f.get('width',1280)*f.get('dpr',1),3)
    if a.shape != expected or b.shape != expected:
        raise RuntimeError(f"Wrong pixel sizes for {f['id']}: expected {expected}, got {a.shape}, {b.shape}")
    d=np.abs(a.astype(np.int16)-b.astype(np.int16)).astype(np.uint8)
    Image.fromarray(d).save(out/'diff'/(f['id']+'.png'))
    Image.fromarray(np.minimum(d.astype(np.uint16)*8,255).astype(np.uint8)).save(out/'diff'/(f['id']+'.x8.png'))
    metric=measure(a,b)
    repeats={side:measure(images[side],images[side+'Repeat']) for side in ('reference','native')}
    # Additional unmasked reading-area diagnostic; never substitutes for the
    # full-window comparison or removes inconvenient foreground pixels.
    scale=f.get('dpr',1); width=f.get('width',1280); height=f.get('height',840)
    left=round(max(240,width*.173)*scale)
    reading=measure(a[48*scale:(height-180)*scale,left:-8*scale],b[48*scale:(height-180)*scale,left:-8*scale])
    content={}
    if f.get('replyMedia'):
        # Same absolute rectangle on both images: union of both actual media
        # component boxes. No translation, resizing, blur, exclusion mask or
        # tolerance. Shell outside the component is reported separately above.
        rects=[]
        for side in ('reference','native'):
            rects+=json.loads((out/side/(f['id']+'.json')).read_text()).get('media',[])
        if rects:
            x0=max(0,int(np.floor(min(r[0] for r in rects)*scale)))
            y0=max(0,int(np.floor(min(r[1] for r in rects)*scale)))
            x1=min(a.shape[1],int(np.ceil(max(r[0]+r[2] for r in rects)*scale)))
            y1=min(a.shape[0],int(np.ceil(max(r[1]+r[3] for r in rects)*scale)))
            crop=lambda image:image[y0:y1,x0:x1]
            content=dict(media_rect=[x0,y0,x1,y1],media_content=measure(crop(a),crop(b)),
                         media_repeat={s:measure(crop(images[s]),crop(images[s+'Repeat'])) for s in ('reference','native')})
            for side in ('reference','native'):
                Image.fromarray(crop(images[side])).save(out/side/(f['id']+'.media.png'))
            Image.fromarray(crop(d)).save(out/'diff'/(f['id']+'.media.png'))
    results.append(dict(id=f['id'],status='headless',classification=category,
                        limited=category==MANUAL,notes=notes,**metric,repeat=repeats,reading_area=reading,**content))
summary=dict(fixtures=len(fixtures),headless=sum(r['status']=='headless' for r in results),
             manual=sum(r['status']=='manual' for r in results),
             exact=sum(r.get('changed_pixels')==0 for r in results),
             differing=sum(r.get('changed_pixels',0)>0 for r in results),
             headless_limited=sum(r.get('limited',False) for r in results),
             repeat_unstable=sum(any(m['changed_pixels'] for m in r.get('repeat',{}).values()) for r in results),
             repeat_over8=sum(any(m['over8_percent']>0 for m in r.get('repeat',{}).values()) for r in results),
             classifications=dict(Counter(r['classification'] for r in results)))
media_results=[r['media_content'] for r in results if 'media_content' in r]
if media_results:
    summary['media_content']=dict(fixtures=len(media_results),exact=sum(m['changed_pixels']==0 for m in media_results),
        changed_percent=100*sum(m['changed_pixels'] for m in media_results)/sum(m['total_pixels'] for m in media_results),
        pixel_weighted_mae=sum(m['mae']*m['total_pixels'] for m in media_results)/sum(m['total_pixels'] for m in media_results),
        median_mae=float(np.median([m['mae'] for m in media_results])),
        repeat_over8=sum(any(m['over8_percent']>0 for m in r.get('media_repeat',{}).values()) for r in results))
report=dict(summary=summary,measurements=results)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
shutil.copyfile(args.manifest,out/'fixtures.json')
fields=['id','status','classification','changed_pixels','total_pixels','changed_percent','over8_percent','mae','rmse','max_channel','limited','notes']
with (out/'measurements.csv').open('w') as stream:
    writer=csv.DictWriter(stream,fieldnames=fields,extrasaction='ignore'); writer.writeheader(); writer.writerows(results)
# Browse artifacts without a running server or GUI launched by this command.
html=['<!doctype html><meta charset="utf-8"><title>OpenGhost visual parity</title><style>body{background:#222;color:#eee;font:14px sans-serif}img{width:32%;vertical-align:top}section{margin:2em 0}pre{white-space:pre-wrap}</style>', '<h1>Fresh headless parity audit</h1>', '<pre>'+json.dumps(summary,indent=2)+'</pre>']
for r in results:
    name=r['id']
    html.append(f'<section><h2>{name}</h2><pre>{escape(json.dumps(r,indent=2))}</pre>')
    if r['status']=='headless':
        html.extend(f'<a href="{folder}/{name}{suffix}.png"><img loading="lazy" src="{folder}/{name}{suffix}.png" alt="{folder}"></a>' for folder,suffix in [('reference',''),('native',''),('diff','.x8')])
    html.append('</section>')
(out/'index.html').write_text('\n'.join(html))
print(json.dumps(summary,indent=2))
if args.strict and (summary['differing'] or summary['manual'] or summary['headless_limited'] or summary['repeat_unstable']):
    raise SystemExit(2)
