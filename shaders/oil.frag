#version 440
// The oil's light (effort-paint.js): the thresholded shape blurred by 1.2 px
// is a height map (surfaceScale 2.2, Sobel normals as SVG lighting takes
// them). feDiffuseLighting in the theme's accent (diffuseConstant 1.4142,
// distant light at azimuth 225°, elevation 45°) plus white
// feSpecularLighting (specularConstant .55, exponent 30, elevation 25°),
// both inside the shape, added as feComposite arithmetic k2 = k3 = 1 does.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 pixel; // One pixel, in texture coordinates.
    vec4 tint; // The diffuse light's colour.
};
layout(binding = 1) uniform sampler2D goo;
layout(binding = 2) uniform sampler2D relief; // goo blurred across (σ 1.2).

// The relief at a neighbour: the vertical half of the 1.2 px blur.
float height(float dx, float dy)
{
    float sum = 0.0, total = 0.0;
    for (int k = -4; k <= 4; ++k) {
        float w = exp(-float(k * k) / 2.88);
        sum += w * texture(relief, qt_TexCoord0 + vec2(dx, dy + float(k)) * pixel).a;
        total += w;
    }
    return sum / total;
}

void main()
{
    float a = height(-1.0, -1.0), b = height(0.0, -1.0), c = height(1.0, -1.0);
    float d = height(-1.0, 0.0), f = height(1.0, 0.0);
    float g = height(-1.0, 1.0), h = height(0.0, 1.0), i = height(1.0, 1.0);
    vec3 n = normalize(vec3(-2.2 / 4.0 * ((c + 2.0 * f + i) - (a + 2.0 * d + g)),
                            -2.2 / 4.0 * ((g + 2.0 * h + i) - (a + 2.0 * b + c)), 1.0));
    vec3 diffuseLight = vec3(-0.5, -0.5, 0.7071068);
    vec3 specularLight = vec3(-0.6408564, -0.6408564, 0.4226183);
    vec3 halfway = normalize(specularLight + vec3(0.0, 0.0, 1.0));
    vec3 shade = clamp(1.4142 * max(dot(n, diffuseLight), 0.0) * tint.rgb, 0.0, 1.0);
    float gloss = clamp(0.55 * pow(max(dot(n, halfway), 0.0), 30.0), 0.0, 1.0);
    float cover = texture(goo, qt_TexCoord0).a;
    float alpha = min(1.0, cover * (1.0 + gloss));
    fragColor = vec4(min(cover * (shade + gloss), vec3(alpha)), alpha) * qt_Opacity;
}
