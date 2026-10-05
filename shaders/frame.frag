#version 440
// OpenGhost 1.3's window backdrop, `background: var(--frame), var(--app-bg)`:
// three radial gradients of light from the left corners over --app-bg
// (theme::Palette::frame, theme::FrameShapes). Each is laid out in `box`
// (the window for the window and pieces pinned to it, as background-
// attachment: fixed; else the item itself); `origin` is where this item
// lies in it. CSS interpolates the stops in premultiplied alpha, so each
// light keeps its colour and only its alpha changes. Like Chromium's
// gradients it is dithered. `mask` cuts it as the sidebar's pieces of it
// are cut (0 none; 1 #000 → transparent at 70 %, the list's top fade; 2
// transparent → #000 at 70 %, its bottom fade; 3 transparent → #000 across,
// a long title's end), with `wash` over it (a row's hover fill), and
// `radius` rounds its corners (antialiased over a pixel).
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 box;
    vec2 origin;
    vec2 size;
    vec4 appBg;
    vec4 light0;
    vec4 alpha0;
    vec4 light1;
    vec4 alpha1;
    vec4 light2;
    vec4 alpha2;
    vec4 wash;
    float mask;
    float radius;
};

// radial-gradient(rx ry at cx cy, …): its ellipse's radii and centre as
// shares of the box, and its stops (the last, transparent, at 1).
float lightAt(vec2 p, vec4 shape, vec3 stops, vec3 alphas)
{
    vec2 d = (p - shape.zw * box) / (shape.xy * box);
    float t = length(d);
    if (t <= stops.y)
        return mix(alphas.x, alphas.y, (t - stops.x) / (stops.y - stops.x));
    if (t <= stops.z)
        return mix(alphas.y, alphas.z, (t - stops.y) / max(stops.z - stops.y, 1e-4));
    if (t < 1.0)
        return mix(alphas.z, 0.0, (t - stops.z) / (1.0 - stops.z));
    return 0.0;
}

void main()
{
    vec2 p = origin + qt_TexCoord0 * size;
    vec3 c = appBg.rgb;
    // The last gradient listed lies lowest.
    c = mix(c, light2.rgb, lightAt(p, vec4(0.26, 0.36, 0.06, 0.5), vec3(0.0, 0.5, 1.0), alpha2.xyz));
    c = mix(c, light1.rgb, lightAt(p, vec4(0.52, 0.64, 0.0, 1.0), vec3(0.0, 0.38, 0.68), alpha1.xyz));
    c = mix(c, light0.rgb, lightAt(p, vec4(0.58, 0.72, 0.0, 0.0), vec3(0.0, 0.38, 0.68), alpha0.xyz));
    c = mix(c, wash.rgb, wash.a);
    // Half a level of noise either way, so no band shows.
    vec2 cell = floor(p * 4.0);
    float n = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    c += n / 255.0;
    float a = 1.0;
    if (mask > 0.5 && mask < 1.5)
        a = 1.0 - clamp(qt_TexCoord0.y / 0.7, 0.0, 1.0);
    else if (mask > 1.5 && mask < 2.5)
        a = clamp(qt_TexCoord0.y / 0.7, 0.0, 1.0);
    else if (mask > 2.5)
        a = qt_TexCoord0.x;
    if (radius > 0.0) {
        vec2 q = abs(qt_TexCoord0 * size - size * 0.5) - (size * 0.5 - radius);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        a *= clamp(0.5 - d, 0.0, 1.0);
    }
    fragColor = vec4(c * a, a) * qt_Opacity;
}
