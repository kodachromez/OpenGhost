(() => {
'use strict';

// Ghosty, the mascot: one picture with a gentle float. look() and blink() are kept for the callers that steered the old
// drawn ghost's eyes; the picture has no eyes to move, so they do nothing for now.
const STYLE = `
:host{display:block}
img{display:block;width:100%;height:100%;object-fit:contain;filter:drop-shadow(0 1px 1.5px rgba(0,0,0,0.18));animation:float 2.6s ease-in-out infinite}
@keyframes float{50%{transform:translateY(-3%)}}
@media (prefers-reduced-motion:reduce){img{animation:none}}
`;

class GhostThinking extends HTMLElement {
 constructor() {
  super();
  this.attachShadow({ mode: 'open' }).innerHTML = `<style>${STYLE}</style><img src="assets/ghosty.png" alt="" draggable="false">`;
 }

 connectedCallback() {
  this.setAttribute('role', 'img');
  this.setAttribute('aria-label', I18n.t('chat.thinking'));
 }

 look() {}

 blink() {}
}

if (!customElements.get('ghost-thinking')) customElements.define('ghost-thinking', GhostThinking);
})();
