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

// Original 5x7 ASCII pixel glyphs: '.', '+', '*', '\\', '/', '-', and '|'.
// No font lookup, texture upload, per-star draw call or neighbor-cell search.
float asciiGlyph(vec2 offset, int glyph, float inkSize) {
    vec2 ink = offset / inkSize + vec2(2.5, 3.5);
    // This canvas is affine: use its known pixel scale instead of derivatives
    // inside per-cell branches (undefined for discontinuous glyph centers).
    vec2 antialias = vec2(max(0.65 / (inkSize * max(uPixelScale, 0.1)), 0.001));
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
    } else if (glyph == 3) {
        if (cell.y >= 1 && cell.y <= 5) row = 1u << uint(cell.y - 1);
    } else if (glyph == 4) {
        if (cell.y >= 1 && cell.y <= 5) row = 1u << uint(5 - cell.y);
    } else if (glyph == 5) {
        if (cell.y == 3) row = 31u;
    } else if (glyph == 6) {
        if (cell.y >= 1 && cell.y <= 5) row = 4u;
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
    // GLES may lower division to a rounded reciprocal. Avoid an extra grid row
    // at exact cell boundaries (e.g. 360 / 30) or density would jump with DPI.
    vec2 gridSize = max(vec2(1.0), ceil(canvas / spacing - vec2(0.0001)));
    float slots = gridSize.x * gridSize.y;
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
    // Much rarer than the star twinkle: first onset 18..42 seconds, then
    // 48..96 seconds between events. No timers or accumulated particle state.
    float epoch = floor(uTime / 72.0);
    uint key = skyHash(uint(epoch) ^ uSeed ^ 0x4d455445u);
    float age = mod(uTime, 72.0) - (18.0 + skyRandom(key) * 24.0);
    float lifetime = 1.3 + skyRandom(key + 6u) * 0.9;
    if (age < 0.0 || age > lifetime) return vec3(0.0);
    float angle = skyRandom(key + 1u) * 6.2831853;
    vec2 direction = vec2(cos(angle), sin(angle));
    vec2 midpoint = canvas * vec2(0.30 + skyRandom(key + 2u) * 0.40,
                                  0.30 + skyRandom(key + 3u) * 0.40);
    // A centered chord in logical space preserves the sampled angle on both
    // portrait and ultrawide outputs, and keeps the entire head path visible.
    float chord = min(canvas.x / max(abs(direction.x), 0.001),
                      canvas.y / max(abs(direction.y), 0.001));
    vec2 travel = direction * chord * (0.28 + skyRandom(key + 4u) * 0.16);
    float size = min(0.7 + skyRandom(key + 5u) * 1.1,
                     max(0.45, min(canvas.x, canvas.y) / 80.0));
    vec2 head = midpoint + travel * (age / lifetime - 0.5);
    // The nearest-segment strip must contain even the diagonal tips of '*'.
    float spacing = 14.0 * size;
    float segment = floor(dot(head - logical, direction) / spacing + 0.5);
    if (segment < 0.0 || segment > 9.0) return vec3(0.0);
    vec2 offset = logical - (head - direction * segment * spacing);
    // Keep ASCII upright, with a slash/line matching the actual trail heading.
    int trailGlyph = abs(direction.y) < abs(direction.x) * 0.4 ? 5 :
                     (abs(direction.x) < abs(direction.y) * 0.4 ? 6 :
                      (direction.x * direction.y >= 0.0 ? 3 : 4));
    int glyph = segment < 0.5 ? 2 : (segment < 5.5 ? trailGlyph : 0);
    float inkSize = (segment < 0.5 ? 1.8 : 1.35) * size;
    float envelope = smoothstep(0.0, 0.16, age) * (1.0 - smoothstep(lifetime - 0.55, lifetime, age));
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
