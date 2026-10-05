#version 440
// CSS brightness() and contrast() on an element's own pixels, after its
// blur (filter: var(--veil) on the composer and the jump button): one map
// c·gain + lift per channel, on unpremultiplied sRGB.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float gain;
    float lift;
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 c = texture(source, qt_TexCoord0);
    if (c.a > 0.0)
        c.rgb = clamp(c.rgb / c.a * gain + lift, 0.0, 1.0) * c.a;
    fragColor = c * qt_Opacity;
}
