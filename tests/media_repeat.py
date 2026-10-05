#!/usr/bin/env python3
"""Bounded, isolated repetitions; record every exit, never retry/hide a crash.

Run with python3, --binary, --runs and --output; --media-test selects unit tests.
Baseline executables must be copied into this worktree, not run in another tree.
This is crash-frequency evidence, not a statistical guarantee of no regression.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--runs', type=int, default=20)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--media-test', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
results = []
for i in range(args.runs):
    with tempfile.TemporaryDirectory(prefix='home-', dir=args.output) as home:
        home = str(Path(home).resolve())
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home+'/config',
                   XDG_DATA_HOME=home+'/data', XDG_CACHE_HOME=home+'/cache',
                   XDG_RUNTIME_DIR=home+'/runtime', DISPLAY='', WAYLAND_DISPLAY='',
                   QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
                   QT_QPA_PLATFORMTHEME='generic', QT_FORCE_STDERR_LOGGING='1',
                   OPENGHOST_REDUCED_MOTION='0', DBUS_SESSION_BUS_ADDRESS='')
        Path(env['XDG_RUNTIME_DIR']).mkdir(mode=0o700)
        command = [str(args.binary.resolve())]
        if not args.media_test:
            command += ['--smoke-test', '--fake-backend']
        with (args.output/f'{i:02}.log').open('w') as log:
            try:
                code = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                      timeout=35).returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        results.append(dict(run=i, exit=code))
        report = dict(binary=str(args.binary), sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                      results=results, passed=sum(r['exit']==0 for r in results),
                      crashes=sum(isinstance(r['exit'],int) and r['exit']<0 for r in results))
        (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
        print(i, code, flush=True)
