#version 440
// One direction of a Gaussian blur (feGaussianBlur). With goo set, the
// blurred alpha then goes through the oil's threshold, effort-paint.js's
// feColorMatrix "0 0 0 22 -9", as white. With tint's alpha set, the blurred
// alpha takes tint's colour instead (a text-shadow's).
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 delta; // One pixel along the blur, in texture coordinates.
    float sigma;
    float goo;
    vec4 tint;
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    // σ 0 (a veil starting) is no blur.
    float s = max(sigma, 0.01);
    float radius = ceil(3.0 * s);
    vec4 sum = vec4(0.0);
    float total = 0.0;
    // Up to σ 18 (3σ = 54 taps), the effort name's glow.
    for (int k = -56; k <= 56; ++k) {
        if (abs(float(k)) > radius)
            continue;
        float w = exp(-float(k * k) / (2.0 * s * s));
        sum += w * texture(source, qt_TexCoord0 + float(k) * delta);
        total += w;
    }
    sum /= total;
    if (goo > 0.5)
        sum = vec4(clamp(22.0 * sum.a - 9.0, 0.0, 1.0));
    if (tint.a > 0.5)
        sum = vec4(tint.rgb * sum.a, sum.a);
    fragColor = sum * qt_Opacity;
}
