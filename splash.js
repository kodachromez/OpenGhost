(() => {
'use strict';

// The one arms-down image moves as a whole with compositor-timed transforms.
// Both the mascot and word rise together; only the mascot overshoots and settles.
const MIST = { show: 300, scale: .6, low: .35, maxWidth: 1080, maxHeight: 650 };
const POP = { at: 120, rise: 600, fall: 480, word: 720, appear: 230, blink: 1360, rest: 1800 };
const START = { compile: 900 };
const HOLD = 420;
const FADE = 620;
const RISE_EASE = 'cubic-bezier(0.333333, 1, 0.666667, 1)';
const FALL_EASE = 'cubic-bezier(0.333333, 0, 0.666667, 1)';
const root = document.documentElement, splash = document.querySelector('.splash');
const reduced = matchMedia('(prefers-reduced-motion: reduce)');
const clamp = v => Math.min(1, Math.max(0, v));
const smooth = t => { t = clamp(t); return t * t * t * (t * (t * 6 - 15) + 10); };
const bar = () => window.Theme?.titleBar();
let stage = null, ended = false, skipNow = false, opening = false, fadeAnimation, release;
const skipped = new Promise(resolve => { release = resolve; });
const wait = ms => Promise.race([new Promise(resolve => setTimeout(resolve, ms)), skipped]);
const onSkip = () => {
 skipNow = true; release();
 if (opening) fadeAnimation?.finish();
};
const onMotion = () => { if (reduced.matches) onSkip(); };
function finish() {
 if (ended) return;
 ended = true;
 release();
 stage?.stop();
 window.removeEventListener('keydown', onSkip);
 reduced.removeEventListener('change', onMotion);
 root.classList.remove('is-splash', 'is-splash-fading');
 splash?.remove();
 bar();
 window.dispatchEvent(new Event('splashend'));
}
if (!splash || !root.classList.contains('is-splash')) { finish(); return; }
// Windows' native caption buttons use the same neutral splash background.
window.openghost?.setTitleBar?.('#000000', '#ffffff');

class Stage {
 constructor(fly, ghost, word, artworkReady) {
  Object.assign(this, { fly, ghost, word, sceneStart: null, poseStart: null, raf: 0, gl: null, landed: false, poseY: 0, presence: 0, motion: [] });
  this.artworkReady = false;
  artworkReady.then(() => { this.artworkReady = true; }).catch(() => {});
  this.canvas = splash.querySelector('.splash-mist');
  this.aura = splash.querySelector('.splash-aura');
  this.mist = window.SplashMist || null;
  this.done = new Promise(resolve => { this.onRest = resolve; });
  this.frame = this.frame.bind(this);
  this.resize = () => this.layout();
  addEventListener('resize', this.resize);
  this.layout();
 }
 setup() {
  if (this.mist) try {
   const { gl, program } = this.mist;
   if (this.mist.failed()) throw new Error('Mist shader unavailable');
   gl.useProgram(program);
   this.buffer = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
   gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
   const corner = gl.getAttribLocation(program, 'corner');
   gl.enableVertexAttribArray(corner); gl.vertexAttribPointer(corner, 2, gl.FLOAT, false, 0, 0);
   const names = ['view', 'density', 'time', 'show', 'pulse', 'count', 'life', 'ghost', 'box', 'trail', 'base', 'night', 'deep', 'lit', 'glow', 'core', 'mistAlpha', 'glowAlpha', 'trailGlow', 'shade', 'motes'];
   this.u = Object.fromEntries(names.map(name => [name, gl.getUniformLocation(program, name)]));
   // Never borrow an app/theme colour, including in light mode.
   for (const [name, value] of [['base', 0], ['night', 0], ['deep', .14], ['lit', .86], ['glow', .88], ['core', 1]]) gl.uniform3f(this.u[name], value, value, value);
   const style = getComputedStyle(splash);
   for (const [name, token] of [['mistAlpha', '--splash-mist-alpha'], ['glowAlpha', '--splash-glow-alpha'], ['trailGlow', '--splash-trail'], ['shade', '--splash-shade'], ['motes', '--splash-motes']]) gl.uniform1f(this.u[name], parseFloat(style.getPropertyValue(token)) || 0);
   gl.uniform1f(this.u.life, .9); gl.uniform1f(this.u.count, 0); gl.uniform4f(this.u.box, -1, -1, -1, -1);
   const info = gl.getExtension('WEBGL_debug_renderer_info'), renderer = info ? String(gl.getParameter(info.UNMASKED_RENDERER_WEBGL)) : '';
   this.quality = /swiftshader|llvmpipe|software/i.test(renderer) ? MIST.low : MIST.scale;
   this.gl = gl;
  } catch { this.mist = null; }
  if (!this.gl) this.canvas.hidden = true;
  else this.canvas.animate({ opacity: [0, 1] }, { duration: 180, fill: 'forwards' });
  this.layout();
 }
 layout() {
  this.w = innerWidth; this.h = innerHeight;
  this.center = { x: this.w / 2, y: this.h * .46 };
  if (!this.gl) return;
  // Smoke is soft; its pixel budget must not balloon with a 4K/high-DPI window.
  const density = Math.min((devicePixelRatio || 1) * this.quality, MIST.maxWidth / this.w, MIST.maxHeight / this.h);
  this.canvas.width = Math.max(1, Math.round(this.w * density));
  this.canvas.height = Math.max(1, Math.round(this.h * density));
  this.gl.viewport(0, 0, this.canvas.width, this.canvas.height);
 }
 start() { this.raf = requestAnimationFrame(this.frame); }
 frame(now) {
  this.raf = 0;
  if (!splash.isConnected) return;
  this.sceneStart ??= now;
  const mistTime = now - this.sceneStart;
  // Shader compilation and artwork decode never hold back the visible CSS smoke.
  if (this.mist && !this.gl) {
   if (this.mist.done()) this.setup();
   else if (mistTime > START.compile) { this.mist = null; this.canvas.hidden = true; }
  }
  if (this.artworkReady && !this.landed) {
   if (this.poseStart === null) this.pop(now);
   this.pose(now - this.poseStart);
  }
  if (this.gl) this.draw(mistTime);
  this.start();
 }
 pop(now) {
  this.poseStart = now;
  this.entry = Math.min(140, this.h * .18);
  this.apex = Math.min(58, this.h * .075);
  this.word.classList.add('is-set');
  const timing = { delay: POP.at, fill: 'both' };
  this.motion = [
   this.fly.animate([
    { transform: `translateY(${this.entry}px)`, offset: 0, easing: RISE_EASE },
    { transform: `translateY(${-this.apex}px)`, offset: POP.rise / (POP.rise + POP.fall), easing: FALL_EASE },
    { transform: 'translateY(0)', offset: 1 }
   ], { ...timing, duration: POP.rise + POP.fall }),
   this.word.animate({ transform: [`translate(-50%, ${this.entry}px)`, 'translate(-50%, 0)'] }, { ...timing, duration: POP.word, easing: RISE_EASE }),
   ...[this.fly, this.word].map(el => el.animate({ opacity: [0, 1] }, { ...timing, duration: POP.appear, easing: 'ease-out' }))
  ];
  // A shared clock makes the image and complete word appear and rise on the same frame.
  for (const animation of this.motion) animation.startTime = now;
 }
 pose(t) {
  const rise = clamp((t - POP.at) / POP.rise);
  const fall = clamp((t - POP.at - POP.rise) / POP.fall);
  const riseProgress = 1 - Math.pow(1 - rise, 3);
  const fallProgress = fall * fall * (3 - 2 * fall);
  // Match the compositor path for the subtle mist halo; never read layout each frame.
  this.poseY = t < POP.at + POP.rise ? this.entry - (this.entry + this.apex) * riseProgress : -this.apex * (1 - fallProgress);
  this.presence = smooth((t - POP.at) / POP.appear);
  const b = t - POP.blink;
  const blink = b < 0 || b > 260 ? 0 : b < 95 ? smooth(b / 95) : b < 145 ? 1 : 1 - smooth((b - 145) / 115);
  this.ghost.render(blink);
  if (t >= POP.rest) this.rest();
 }
 rest() {
  this.landed = true;
  for (const animation of this.motion) animation.finish();
  this.fly.style.transform = 'none'; this.fly.style.opacity = '1';
  this.word.classList.add('is-set'); this.word.style.opacity = '1';
  this.poseY = 0; this.presence = 1;
  this.ghost.render(); this.onRest();
 }
 draw(t) {
  const gl = this.gl, u = this.u;
  // No layout reads after pose writes. The centre and rise offset already describe the artwork.
  const x = this.center.x, y = this.center.y + this.poseY;
  gl.uniform2f(u.view, this.w, this.h); gl.uniform1f(u.density, this.canvas.width / this.w);
  gl.uniform1f(u.time, t / 1000); gl.uniform1f(u.show, smooth(t / MIST.show));
  gl.uniform1f(u.pulse, Math.max(0, t / 1000 - .4));
  gl.uniform4f(u.ghost, x, y, 1, this.presence);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
 }
 skip() { this.rest(); }
 stop() {
  cancelAnimationFrame(this.raf);
  for (const animation of this.motion) animation.cancel();
  removeEventListener('resize', this.resize);
  if (this.gl && this.buffer) this.gl.deleteBuffer(this.buffer);
 }
}

async function fade(duration) {
 opening = true;
 splash.classList.add('is-opening');
 root.classList.add('is-splash-fading');
 // Fade the entire composition in place; the app's own mascot stays independent.
 fadeAnimation = splash.animate({ opacity: [1, 0] }, { duration, easing: 'ease-in-out', fill: 'forwards' });
 await fadeAnimation.finished;
}

async function play() {
 const fly = splash.querySelector('.splash-fly'), word = splash.querySelector('.splash-word'), ghost = splash.querySelector('splash-ghost');
 await customElements.whenDefined('splash-ghost');
 // Smoke starts independently, but both visible subjects must be ready for the shared entrance.
 const artworkReady = Promise.all([ghost.ready, document.fonts.load('700 48px "Ghosty Rounded"').catch(() => {})]);
 if (reduced.matches) {
  await Promise.race([artworkReady, skipped]);
  if (!skipNow) {
   splash.classList.add('is-static'); ghost.render();
   await wait(650);
   await fade(200);
  }
  finish(); return;
 }
 stage = new Stage(fly, ghost, word, artworkReady); stage.start();
 artworkReady.catch(error => { console.error('Splash artwork:', error); finish(); });
 const wasSkipped = await Promise.race([stage.done.then(() => false), skipped.then(() => true)]);
 if (ended) return;
 if (wasSkipped) stage.skip();
 await wait(HOLD);
 if (ended) return;
 await fade(reduced.matches || skipNow ? 200 : FADE);
 finish();
}

splash.addEventListener('pointerdown', onSkip);
window.addEventListener('keydown', onSkip, { once: true });
reduced.addEventListener('change', onMotion);
play().catch(error => { console.error('Splash:', error); finish(); });
})();
