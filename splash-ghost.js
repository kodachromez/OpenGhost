(() => {
'use strict';

// Splash-only, fixed arms-down artwork. Position and opacity are animated by the
// splash container; this component never moves, deforms or changes the mascot's arms.
const SIZE = 512;
const clamp = n => Math.max(0, Math.min(1, n));
const smooth = n => { n = clamp(n); return n * n * (3 - 2 * n); };

class SplashGhost extends HTMLElement {
 constructor() {
  super();
  this.attachShadow({ mode: 'open' }).innerHTML = '<style>:host{display:block}canvas{display:block;width:100%;height:100%}</style>';
  this.canvas = document.createElement('canvas');
  this.canvas.width = this.canvas.height = SIZE;
  this.shadowRoot.append(this.canvas);
  this.ctx = this.canvas.getContext('2d');
  this.ready = this.load();
 }

 async load() {
  // Neutralization, background cleanup and uniform registration are baked in.
  // No image processing or repeated canvas painting is needed while the mascot rises.
  const image = new Image();
  image.src = 'assets/splash-poses/down.png';
  await image.decode();
  this.artwork = image;
  this.render();
 }

 render(blink = 0) {
  if (!this.artwork) return;
  blink = clamp(blink);
  if (this.previousBlink === blink) return;
  this.previousBlink = blink;
  const c = this.ctx;
  c.clearRect(0, 0, SIZE, SIZE);
  c.drawImage(this.artwork, 0, 0);
  if (blink > 0) this.eyelids(blink);
 }

 eyelids(amount) {
  const c = this.ctx;
  for (const [x, y, rx, ry] of [[219, 149, 25, 33], [338, 173, 24, 32]]) {
   c.save(); c.translate(x, y); c.rotate(.28);
   c.beginPath(); c.ellipse(0, 0, rx, ry, 0, 0, Math.PI * 2); c.clip();
   const fabric = c.createLinearGradient(-rx, 0, rx, 0);
   fabric.addColorStop(0, '#f5f5f5'); fabric.addColorStop(1, '#fafafa');
   c.fillStyle = fabric;
   const opening = ry * (1 - amount);
   // A rounded aperture closes over the original eye pixels; no eye texture is scaled.
   // Straight rectangular lids would leave a harsh black bar halfway through the blink.
   c.save(); c.beginPath(); c.rect(-rx, -ry, rx * 2, ry * 2);
   if (opening > .1) c.ellipse(0, 0, rx, opening, 0, 0, Math.PI * 2);
   c.clip('evenodd'); c.fillRect(-rx, -ry, rx * 2, ry * 2); c.restore();
   if (amount > .75) {
    c.globalAlpha = smooth((amount - .75) / .25);
    c.strokeStyle = '#242424'; c.lineWidth = 3.4; c.lineCap = 'round';
    c.beginPath(); c.moveTo(-16, -1); c.quadraticCurveTo(0, 9, 16, -1); c.stroke();
   }
   c.restore();
  }
 }
}

customElements.define('splash-ghost', SplashGhost);
})();
