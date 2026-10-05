#version 440
// The selection's veil (selection-focus.js): the feed behind
// backdrop-filter: blur(5px) brightness() contrast(), cut out where the
// mask is lit. The sharp and blurred feeds are over the chat background
// first, as the backdrop is; the two filters, at the veil's progress, are
// one map c·gain + lift per channel (sRGB, as Chromium filters). Drawn
// only inside the panel's rounded inner edge.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 background;
    float gain;
    float lift;
    vec2 extent; // The item, in pixels.
    vec4 inner; // The rounded inner edge: x, y, width, height.
    float radius;
};
layout(binding = 1) uniform sampler2D sharp;
layout(binding = 2) uniform sampler2D blurred;
layout(binding = 3) uniform sampler2D mask;

void main()
{
    vec4 s = texture(sharp, qt_TexCoord0);
    vec4 b = texture(blurred, qt_TexCoord0);
    vec3 plain = s.rgb + background.rgb * (1.0 - s.a);
    vec3 veiled = clamp((b.rgb + background.rgb * (1.0 - b.a)) * gain + lift, 0.0, 1.0);
    float cut = texture(mask, qt_TexCoord0).a;
    vec3 color = mix(veiled, plain, cut);
    vec2 p = qt_TexCoord0 * extent;
    vec2 half_ = inner.zw * 0.5;
    vec2 q = abs(p - inner.xy - half_) - (half_ - radius);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float cover = clamp(0.5 - d, 0.0, 1.0);
    fragColor = vec4(color, 1.0) * cover * qt_Opacity;
}
