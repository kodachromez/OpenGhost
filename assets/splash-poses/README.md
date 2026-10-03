# Splash artwork

`down.png` is a neutral, transparent, uniformly registered copy of the supplied `assets/arms-down.png`. Both arms stay down throughout the splash. The supplied original is unchanged.

Registration uses one uniform scale, rotation and translation. Background-connected dark pixels were converted to neutral alpha, the image was converted to grayscale, and faint background speckle/halo was suppressed offline. Opaque mascot pixels retain the actual supplied artwork. No skew, separate-axis scaling, deformation, generated arms or pose blending is used.

`splash-ghost.js` loads this single image and exposes `render(blink = 0)`. Repeated identical calls do not redraw the canvas. The splash container controls the rise, small settling drop and complete fade into the app; optional eyelids provide the existing blink without changing the arms.
