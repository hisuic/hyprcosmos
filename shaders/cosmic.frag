#version 300 es
precision highp float;
precision highp int;

in vec2 vUV;
in vec2 vLocal;
in vec2 vScreen;
uniform sampler2D uTexture;
uniform int uMode;
uniform vec4 uColor;
uniform float uTime;
uniform float uRadius;
uniform float uPhase;
uniform float uStars;
uniform float uBackground;
uniform vec2 uResolution;
// A local sky canvas in physical pixels. Glyph metrics stay in logical pixels
// so a 2x output has the same readable ASCII shapes, not half-sized stars.
uniform vec4 uSkyViewport;
uniform float uPixelScale;
uniform uint uSeed;
uniform vec4 uClip;
layout(location = 0) out vec4 fragColor;
// Hyprland attaches an unmodified mirror at COLOR_ATTACHMENT1 for screencopy
// and mirrored outputs. Leaving it unwritten produces stale/undefined captures.
// Without a mirror attachment the second output is safely discarded by GLES.
layout(location = 1) out vec4 mirrorColor;

uint skyHash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float skyRandom(uint value) {
    return float(skyHash(value) >> 8u) / 16777216.0;
}

// Original 5x7 ASCII pixel glyphs: '.', '+', '*', and '\\' for meteor tails.
// No font lookup, texture upload, per-star draw call or neighbor-cell search.
float asciiGlyph(vec2 offset, int glyph, float inkSize) {
    vec2 ink = offset / inkSize + vec2(2.5, 3.5);
    // Derivatives must be evaluated before glyph-specific early returns.
    vec2 antialias = max(fwidth(ink) * 0.65, vec2(0.001));
    ivec2 cell = ivec2(floor(ink));
    if (cell.x < 0 || cell.x >= 5 || cell.y < 0 || cell.y >= 7) return 0.0;
    uint row = 0u;
    if (glyph == 0) {
        if (cell.y == 3 || cell.y == 4) row = 4u;
    } else if (glyph == 1) {
        if (cell.y >= 1 && cell.y <= 5) row = cell.y == 3 ? 31u : 4u;
    } else if (glyph == 2) {
        if (cell.y == 1 || cell.y == 5) row = 17u;
        if (cell.y == 2 || cell.y == 4) row = 10u;
        if (cell.y == 3) row = 31u;
    } else {
        if (cell.y >= 1 && cell.y <= 5) row = 1u << uint(cell.y - 1);
    }
    if ((row & (1u << uint(cell.x))) == 0u) return 0.0;
    vec2 edge = min(fract(ink), 1.0 - fract(ink));
    vec2 coverage = smoothstep(vec2(0.0), antialias, edge);
    return coverage.x * coverage.y;
}

vec3 asciiStars(vec2 logical, vec2 canvas) {
    if (uStars <= 0.0) return vec3(0.0);
    const vec2 spacing = vec2(34.0, 30.0);
    vec2 cell = floor(logical / spacing);
    uint key = skyHash(uint(cell.x) ^ (uint(cell.y) * 0x9e3779b9u) ^ uSeed);
    float slots = max(1.0, ceil(canvas.x / spacing.x) * ceil(canvas.y / spacing.y));
    if (skyRandom(key) >= min(1.0, uStars / slots)) return vec3(0.0);
    vec2 center = (cell + 0.5) * spacing;
    center += vec2(skyRandom(key + 1u) - 0.5, skyRandom(key + 2u) - 0.5) * vec2(18.0, 12.0);
    float tier = skyRandom(key + 3u);
    int glyph = tier < 0.38 ? 0 : (tier < 0.76 ? 1 : 2);
    float inkSize = glyph == 0 ? 1.2 : (glyph == 1 ? 1.45 : 1.7);
    float brightness = mix(0.32, 0.86, tier);
    float twinkle = 0.8 + 0.2 * sin(uTime * (0.45 + skyRandom(key + 4u) * 0.65)
                                   + skyRandom(key + 5u) * 6.2831853);
    vec3 tint = mix(vec3(0.51, 0.67, 0.90), vec3(0.80, 0.89, 1.0), tier);
    if (skyRandom(key + 6u) > 0.94) tint = vec3(0.90, 0.78, 0.62);
    return tint * brightness * twinkle * asciiGlyph(logical - center, glyph, inkSize);
}

vec3 asciiMeteor(vec2 logical, vec2 canvas) {
    if (uStars <= 0.0) return vec3(0.0);
    // One bounded event per epoch, with randomized onset: no global RNG,
    // timers, accumulated particles, or state to leak when Cosmic stops.
    float epoch = floor(uTime / 12.0);
    uint key = skyHash(uint(epoch) ^ uSeed ^ 0x4d455445u);
    float age = mod(uTime, 12.0) - (3.0 + skyRandom(key) * 4.0);
    if (age < 0.0 || age > 1.8) return vec3(0.0);
    vec2 origin = canvas * vec2(0.12 + skyRandom(key + 1u) * 0.20,
                                0.12 + skyRandom(key + 2u) * 0.20);
    vec2 travel = canvas * vec2(0.52, 0.24);
    vec2 direction = normalize(travel);
    vec2 head = origin + travel * (age / 1.8);
    float segment = floor(dot(head - logical, direction) / 12.0 + 0.5);
    if (segment < 0.0 || segment > 9.0) return vec3(0.0);
    vec2 offset = logical - (head - direction * segment * 12.0);
    int glyph = segment < 0.5 ? 2 : (segment < 5.5 ? 3 : 0);
    float inkSize = segment < 0.5 ? 1.8 : 1.35;
    float envelope = smoothstep(0.0, 0.16, age) * (1.0 - smoothstep(1.25, 1.8, age));
    float trail = pow(1.0 - segment / 10.0, 1.6);
    return vec3(0.70, 0.87, 1.0) * envelope * trail * asciiGlyph(offset, glyph, inkSize);
}

void main() {
    vec2 pixel = vScreen * uResolution;
    if (any(lessThan(pixel, uClip.xy)) || any(greaterThan(pixel, uClip.zw))) discard;
    if (uMode == 0) {
        vec2 p = (pixel - uSkyViewport.xy) / max(uSkyViewport.zw, vec2(1.0));
        vec2 logical = (pixel - uSkyViewport.xy) / max(uPixelScale, 0.1);
        vec2 canvas = uSkyViewport.zw / max(uPixelScale, 0.1);
        float aspect = canvas.x / max(canvas.y, 1.0);
        vec2 q = (p - 0.5) * vec2(aspect, 1.0);
        float cloud = exp(-dot(q - vec2(-0.36, -0.08), q - vec2(-0.36, -0.08)) * 2.8);
        float band = exp(-pow(q.y - q.x * 0.20 + 0.09, 2.0) * 36.0);
        float teal = exp(-dot(q - vec2(0.58, 0.20), q - vec2(0.58, 0.20)) * 4.0);
        vec3 color = vec3(0.008, 0.012, 0.027);
        color += vec3(0.018, 0.013, 0.041) * cloud;
        color += vec3(0.007, 0.012, 0.025) * band;
        color += vec3(0.002, 0.013, 0.016) * teal;
        color *= 1.0 - 0.28 * smoothstep(0.3, 1.2, length(q));
        color += asciiStars(logical, canvas) + asciiMeteor(logical, canvas);
        fragColor = vec4(clamp(color, 0.0, 1.0) * uBackground, uBackground);
    } else if (uMode == 1) {
        if (any(lessThan(vUV, vec2(0.0))) || any(greaterThan(vUV, vec2(1.0)))) discard;
        vec4 texel = texture(uTexture, vUV);
        // Captured framebuffer pixels already use premultiplied alpha.
        fragColor = texel * uColor.a;
    } else if (uMode == 2) {
        float r = length(vLocal) * 2.0;
        float a = atan(vLocal.y, vLocal.x);
        float width = max(0.008, uRadius);
        float ring = exp(-pow((r - 0.76) / width, 2.0));
        float glow = exp(-pow((r - 0.76) / (width * 4.0), 2.0)) * 0.23;
        float streak = 0.55 + 0.45 * sin(a * 5.0 - uTime * 2.0 + uPhase);
        float alpha = (ring * streak + glow) * uColor.a;
        // Dark center makes wormhole mouths readable against bright windows.
        float core = (1.0 - smoothstep(0.42, 0.67, r)) * uPhase;
        vec3 color = uColor.rgb * alpha;
        alpha = max(alpha, core * 0.88 * uColor.a);
        fragColor = vec4(color, alpha);
    } else if (uMode == 4) {
        fragColor = vec4(uColor.rgb * uColor.a, uColor.a);
    } else {
        float r = length(vLocal) * 2.0;
        float alpha = exp(-r * r * 5.0) * uColor.a;
        fragColor = vec4(uColor.rgb * alpha, alpha);
    }
    mirrorColor = fragColor;
}
