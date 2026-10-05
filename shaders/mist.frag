#version 440
// The splash's night and mist (OpenGhost 1.3 splash-mist.js), in one pass:
// the night falling over --chat-bg, mist lit from the middle and by the
// Ghost, the trail it leaves in the mist, the ring of light at the landing
// and the motes in the air. The fragment code is splash-mist.js's FRAGMENT,
// with gl_FragCoord / density read as qt_TexCoord0 · view. A ShaderEffect
// has no uniform arrays, so the trail's points are not passed in: each is
// worked out here from splash.js's flight (pose()) at its own time, exactly
// as splash.js works them out before it passes them. `scene` is the scene's
// time in ms, `trailEnd` the last moment of the way taken (min(t, landing))
// and `count` how many of the KEEP points splash.js would pass.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 view;
    float time;
    float show;
    float pulse;
    float count;
    float life;
    float scene;
    float trailEnd;
    vec4 ghost;
    vec4 box;
    vec4 base;
    vec4 night;
    vec4 deep;
    vec4 lit;
    vec4 glow;
    vec4 core;
    float mistAlpha;
    float glowAlpha;
    float trailGlow;
    float shade;
    float motes;
};

// splash.js: FLIGHT and TRAIL, and splash-mist.js's KEEP.
const int KEEP = 24;
const float FLIGHT_AT = 220.0, FLIGHT_DURATION = 1650.0, FLIGHT_FROM = 128.0, FLIGHT_TURN = 335.0;
const float FLIGHT_REACH = 0.56, FLIGHT_POWER = 1.3, FLIGHT_EASE = 2.2, FLIGHT_APPEAR = 0.16;
const float TRAIL_BUNCH = 1.6;

// splash.js pose() and spot(): where the Ghost was at scene time `when`, and
// the point's age (its z).
vec3 trailPoint(int k)
{
    float when = trailEnd - life * 1000.0 * pow(float(k) / float(KEEP - 1), TRAIL_BUNCH);
    float q = clamp((when - FLIGHT_AT) / FLIGHT_DURATION, 0.0, 1.0);
    float u = 1.0 - pow(1.0 - q, FLIGHT_EASE);
    float angle = (FLIGHT_FROM + FLIGHT_TURN * u) * 3.14159265358979 / 180.0;
    float r = FLIGHT_REACH * view.x * pow(1.0 - u, FLIGHT_POWER);
    vec2 at = view * 0.5 + vec2(cos(angle) * r, sin(angle) * r * view.y / view.x * 1.15);
    float a = clamp(q / FLIGHT_APPEAR, 0.0, 1.0);
    float presence = a * a * (3.0 - 2.0 * a);
    return vec3(at, (scene - when) / 1000.0 + (1.0 - presence) * life);
}

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = mat2(1.6, 1.2, -1.2, 1.6) * p;
        a *= 0.5;
    }
    return v;
}

void main()
{
    vec2 px = qt_TexCoord0 * view;
    vec2 c = px - view * 0.5;

    // The trail: mist lit from within where the Ghost has just flown, measured
    // from the nearest point of the whole way; the mist parts around it.
    float band = 0.0;
    vec2 push = vec2(0.0);
    if (px.x > box.x && px.y > box.y && px.x < box.z && px.y < box.w) {
        float best = 1e8, age = 9.0;
        vec2 away = vec2(0.0);
        vec3 a = trailPoint(0);
        for (int i = 0; i < KEEP - 1; i++) {
            if (float(i) >= count - 1.0)
                break;
            vec3 b = trailPoint(i + 1);
            vec2 ab = b.xy - a.xy;
            float h = clamp(dot(px - a.xy, ab) / max(dot(ab, ab), 0.001), 0.0, 1.0);
            vec2 d = px - a.xy - ab * h;
            float dd = dot(d, d);
            if (dd < best) {
                best = dd;
                age = mix(a.z, b.z, h);
                away = d;
            }
            a = b;
        }
        float width = 32.0 + 110.0 * age;
        band = exp(-best / (width * width)) * smoothstep(0.0, 0.14, age) * clamp(1.0 - age / life, 0.0, 1.0);
        push = away / width * band;
        band *= 0.55 + 0.7 * fbm((px + push * 20.0) / 72.0 + vec2(time * 0.5, -time * 0.3));
    }

    // The mist: noise folded into itself, drifting slowly, thicker where the trail hangs.
    vec2 p = (px + push * 60.0) / 300.0;
    vec2 warp = vec2(fbm(p + vec2(0.0, time * 0.06)), fbm(p + vec2(5.2, 1.3) - vec2(time * 0.05, 0.0)));
    float fog = smoothstep(0.32, 0.9, fbm(p * 1.4 + warp * 1.8 + vec2(time * 0.025, -time * 0.015)));
    fog = clamp(fog + band * 0.55, 0.0, 1.0);

    // The light: the middle, the Ghost's own glow, the trail, and the landing's ring.
    vec2 g = px - ghost.xy;
    float near = exp(-dot(g, g) / (170.0 * 170.0 * ghost.z * ghost.z)) * ghost.w;
    float middle = exp(-dot(c, c) / (440.0 * 440.0));
    float ring = 0.0;
    if (pulse > 0.0) {
        float k = length(c) - pulse * 520.0;
        ring = exp(-k * k / 3600.0) * max(0.0, 1.0 - pulse / 1.4);
    }
    float light = 0.35 + 0.65 * middle + 1.2 * near + 1.1 * band + 0.6 * ring;

    // Motes of light, rising slowly and twinkling, brighter where the light is.
    vec2 q = px / 52.0 + vec2(0.0, time * 0.22);
    vec2 cell = floor(q), f = fract(q);
    float seed = hash(cell);
    vec2 dm = (f - vec2(hash(cell + 3.1), hash(cell + 7.7)) * 0.8 - 0.1) * 52.0;
    float mote = step(0.86, seed) * exp(-dot(dm, dm) / 3.0)
                 * (0.5 + 0.5 * sin(time * (1.3 + seed * 2.4) + seed * 40.0)) * (0.35 + light * 0.5);

    vec3 color = mix(base.rgb, night.rgb, show);
    color = mix(color, mix(deep.rgb, lit.rgb, clamp(light * 0.55, 0.0, 1.0)), clamp(fog * mistAlpha * show, 0.0, 1.0));
    color = mix(color, glow.rgb, clamp((near * 0.55 + band * trailGlow + ring * 0.3 + middle * 0.1) * glowAlpha * show, 0.0, 1.0));
    color = mix(color, core.rgb, clamp(mote * motes * show, 0.0, 1.0));
    color *= 1.0 - shade * show * smoothstep(0.55, 1.35, length(c / (view * 0.5)));
    fragColor = vec4(color, 1.0) * qt_Opacity;
}
