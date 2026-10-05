#version 440
// A glass lens (liquid-glass.js and .glass-lens): the backdrop magnified
// `magnify` × about the lens's centre and bent inward within `band` px of its
// rim (by up to `bend` px; liquid-glass.js's zoom, band and edge: 1.15, 7 and
// 6 for the effort's lens, 1.1, 5 and 3.5 for a dock's), then blur(0.4px)
// saturate() brightness() (--glass-backdrop), with
// --glass-fill over it, inside the pill. The lens may be scaled about its
// centre; the backdrop is read where each point of it lies.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size; // The lens, unscaled.
    vec2 origin; // Its top left in the backdrop, unscaled.
    vec2 zoom; // Its scale about its centre.
    vec2 backdropSize;
    float saturation;
    float brightness;
    vec4 fillTop; // --glass-fill at 0, 55 % and 100 %, unpremultiplied.
    vec4 fillMiddle;
    vec4 fillBottom;
    float magnify;
    float bend;
    float band;
};
layout(binding = 1) uniform sampler2D backdrop;

vec2 displacement(vec2 p)
{
    vec2 c = size * 0.5;
    float radius = min(c.x, c.y);
    vec2 q = abs(p - c) - (c - radius);
    vec2 n;
    float depth;
    if (q.x > 0.0 && q.y > 0.0) {
        float len = length(q);
        n = q / len;
        depth = radius - len;
    } else if (q.x > q.y) {
        n = vec2(1.0, 0.0);
        depth = radius - q.x;
    } else {
        n = vec2(0.0, 1.0);
        depth = radius - q.y;
    }
    n *= vec2(p.x < c.x ? -1.0 : 1.0, p.y < c.y ? -1.0 : 1.0);
    float pushed = bend * pow(1.0 - clamp(max(depth, 0.0) / band, 0.0, 1.0), 2.0);
    return (c - p) * (1.0 - 1.0 / magnify) - n * pushed;
}

vec4 refracted(vec2 p)
{
    vec2 q = p + displacement(p);
    vec2 at = origin + size * 0.5 + (q - size * 0.5) * zoom;
    return texture(backdrop, at / backdropSize);
}

void main()
{
    vec2 p = qt_TexCoord0 * size;
    // blur(0.4px): a 3 × 3 Gaussian.
    const float side = 0.0439369; // exp(-1 / (2 × 0.4²))
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            float w = (i == 0 ? 1.0 : side) * (j == 0 ? 1.0 : side);
            sum += w * refracted(p + vec2(float(i), float(j)));
            total += w;
        }
    }
    vec4 seen = sum / total;
    vec3 rgb = seen.a > 0.0 ? seen.rgb / seen.a : vec3(0.0);
    float s = saturation;
    rgb = clamp(mat3(0.213 + 0.787 * s, 0.213 - 0.213 * s, 0.213 - 0.213 * s,
                     0.715 - 0.715 * s, 0.715 + 0.285 * s, 0.715 - 0.715 * s,
                     0.072 - 0.072 * s, 0.072 - 0.072 * s, 0.072 + 0.928 * s) * rgb, 0.0, 1.0);
    rgb = clamp(rgb * brightness, 0.0, 1.0);
    float y = qt_TexCoord0.y;
    vec4 fill = y < 0.55 ? mix(fillTop, fillMiddle, y / 0.55) : mix(fillMiddle, fillBottom, (y - 0.55) / 0.45);
    vec4 glass = vec4(fill.rgb * fill.a, fill.a) + vec4(rgb * seen.a, seen.a) * (1.0 - fill.a);
    // The pill, antialiased.
    vec2 c = size * 0.5;
    float radius = min(c.x, c.y);
    vec2 q = abs(p - c) - (c - radius);
    float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float cover = clamp(0.5 - outside * min(zoom.x, zoom.y), 0.0, 1.0);
    fragColor = glass * cover * qt_Opacity;
}
