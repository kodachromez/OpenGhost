(() => {
'use strict';

// Start compilation while the app loads; splash.js keeps ownership of drawing and handoff.
const root = document.documentElement;
const canvas = document.querySelector('.splash-mist');
if (!canvas || !root.classList.contains('is-splash') || window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;

// Preserve the existing draw interface, including uniforms that this restrained opening no longer needs.
const KEEP = 24;
const VERTEX = 'attribute vec2 corner; void main() { gl_Position = vec4(corner, 0.0, 1.0); }';
const FRAGMENT = `
precision highp float;
uniform vec2 view;
uniform float density, time, show, pulse, count, life;
uniform vec4 ghost, box;
uniform vec3 trail[${KEEP}];
uniform vec3 base, night, deep, lit, glow, core;
uniform float mistAlpha, glowAlpha, trailGlow, shade, motes;

float hash(vec2 p) {
 p = fract(p * vec2(123.34, 456.21));
 p += dot(p, p + 45.32);
 return fract(p.x * p.y);
}

float noise(vec2 p) {
 vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
 return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p) {
 float v = 0.0, a = 0.5;
 for (int i = 0; i < 5; i++) {
  v += a * noise(p);
  p = mat2(1.6, 1.2, -1.2, 1.6) * p;
  a *= 0.5;
 }
 return v;
}

void main() {
 if (show <= 0.0) {
  gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
  return;
 }
 vec2 px = vec2(gl_FragCoord.x, view.y * density - gl_FragCoord.y) / density;
 vec2 uv = px / view;
 float unit = max(280.0, min(view.x, view.y));
 float light = 0.0;
 vec2 ringCenter = view * vec2(0.5, 0.78);
 float radius = clamp(view.x * 0.18, 110.0, 210.0);

 // Above this band the smoke envelope is exactly zero. Keep its full five-octave detail only where visible;
 // skipping all twenty noise evaluations across the upper stage is the largest high-refresh saving.
 // The ring bound also covers unusually short windows without cutting off its soft haze.
 if (uv.y > 0.475 || abs(px.y - ringCenter.y) < radius * 0.36) {
  // Fold broad, slowly opposing currents into fine wisps. The uneven envelope and stretched noise give the
  // lower smoke depth without a solid grey floor.
  vec2 p = vec2(px.x, px.y * 1.8) / (unit * 0.46);
  vec2 warp = vec2(fbm(p + vec2(time * 0.11, 0.0)), fbm(p + vec2(5.2, 1.3) - vec2(time * 0.075, time * 0.04)));
  float broad = fbm(p + warp * 2.0 + vec2(time * 0.055, -time * 0.045));
  float fine = fbm(p * 2.4 + warp * 1.8 - vec2(time * 0.09, time * 0.03));
  float veil = smoothstep(0.54, 0.86, uv.y + (broad - 0.5) * 0.13);
  float floorFade = 1.0 - smoothstep(0.90, 1.15, uv.y);
  float horizontal = 1.0 - smoothstep(0.14, 0.64, abs(uv.x - 0.5));
  float fog = smoothstep(0.31, 0.76, broad * 0.62 + fine * 0.38) * veil * floorFade;
  fog *= 0.42 + horizontal * 0.58;

  // An elliptical ring gathers before emergence. Noise softens its silver-grey rim into smoke.
  vec2 ringPoint = (px - ringCenter) / vec2(radius, radius * 0.18);
  float ringDistance = length(ringPoint) - 1.0 - (fine - 0.5) * 0.08;
  float ring = exp(-ringDistance * ringDistance / 0.014);
  ring *= smoothstep(0.10, 0.45, time) * (0.52 + broad * 0.48);
  float ringHaze = exp(-ringDistance * ringDistance / 0.16) * 0.035;
  ringHaze *= smoothstep(0.10, 0.45, time);
  light = fog * (0.28 + horizontal * 0.12) * mistAlpha + ring * 0.15 + ringHaze;
 }

 // A compact neutral halo follows the mascot, with an explicit cutoff so the corners stay pure black.
 vec2 offset = px - ghost.xy;
 float haloRadius = unit * 0.25 * max(ghost.z, 0.55);
 if (ghost.w > 0.0 && dot(offset, offset) < haloRadius * haloRadius) {
  float near = 1.0 - smoothstep(0.0, 1.0, length(offset) / haloRadius);
  light += near * near * ghost.w * glowAlpha * 0.06;
 }

 // Sparse white motes near the mascot, rising slowly. Only their small circular cluster needs hash/trig work;
 // empty cells (91 percent of the grid) also skip the sparkle's exponentials.
 float sparkleRadius = unit * 0.27;
 if (ghost.w > 0.0 && motes > 0.0 && time > 1.3 && dot(offset, offset) < sparkleRadius * sparkleRadius * 1.8225) {
  vec2 q = px / 60.0 + vec2(0.0, time * 0.10);
  vec2 cell = floor(q), f = fract(q);
  float seed = hash(cell);
  if (seed >= 0.91) {
   vec2 dm = (f - vec2(hash(cell + 3.1), hash(cell + 7.7)) * 0.8 - 0.1) * 60.0;
   float dotLight = exp(-dot(dm, dm) / 1.5);
   float crossLight = exp(-abs(dm.x) * 2.8 - abs(dm.y) * 0.65) + exp(-abs(dm.y) * 2.8 - abs(dm.x) * 0.65);
   float sparkleArea = 1.0 - smoothstep(0.25, 1.35, length(offset) / sparkleRadius);
   float twinkle = 0.5 + 0.5 * sin(time * (1.1 + seed) + seed * 40.0);
   float mote = (dotLight + crossLight * 0.35) * twinkle * twinkle * sparkleArea;
   light += mote * smoothstep(1.3, 2.0, time) * ghost.w * motes * 0.8;
  }
 }

 // One scalar feeds all three channels: no theme colour can introduce a hue, and show=0 is exactly black.
 light = clamp(light * show, 0.0, 1.0);
 gl_FragColor = vec4(vec3(light), 1.0);
}`;

const gl = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false, powerPreference: 'high-performance' });
if (!gl) return;
const parallel = gl.getExtension('KHR_parallel_shader_compile');
const program = gl.createProgram();
const shaders = [[gl.VERTEX_SHADER, VERTEX], [gl.FRAGMENT_SHADER, FRAGMENT]].map(([type, source]) => {
 const shader = gl.createShader(type);
 gl.shaderSource(shader, source);
 gl.compileShader(shader);
 gl.attachShader(program, shader);
 return shader;
});
gl.linkProgram(program);

window.SplashMist = {
 gl, program, keep: KEEP,
 done: () => !parallel || gl.getProgramParameter(program, parallel.COMPLETION_STATUS_KHR),
 failed: () => gl.getProgramParameter(program, gl.LINK_STATUS) ? '' : gl.getProgramInfoLog(program) || shaders.map(s => gl.getShaderInfoLog(s)).join(' ') || 'link failed',
};
})();
