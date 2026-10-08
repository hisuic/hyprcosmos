#version 300 es
precision highp float;

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
out vec4 fragColor;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    if (uMode == 0) {
        vec2 p = vScreen;
        float aspect = uResolution.x / max(uResolution.y, 1.0);
        vec2 q = (p - 0.5) * vec2(aspect, 1.0);
        float cloud = exp(-dot(q - vec2(-0.2, 0.04), q - vec2(-0.2, 0.04)) * 2.7);
        float band = exp(-pow(q.y + sin(q.x * 2.4) * 0.15, 2.0) * 24.0);
        vec3 color = vec3(0.009, 0.012, 0.033);
        color += vec3(0.022, 0.016, 0.072) * cloud;
        color += vec3(0.014, 0.022, 0.045) * band;
        // Deterministic stars with a tiny twinkle, without texture allocation.
        vec2 grid = p * vec2(aspect, 1.0) * 180.0;
        vec2 cell = floor(grid);
        vec2 spot = fract(grid) - vec2(hash(cell), hash(cell + 17.0));
        float rarity = step(1.0 - uStars / (aspect * 32400.0), hash(cell + 7.0));
        float star = exp(-dot(spot, spot) * 650.0) * rarity;
        float twinkle = 0.68 + 0.32 * sin(uTime * (0.3 + hash(cell) * 0.9) + hash(cell + 3.0) * 6.28);
        color += vec3(0.62, 0.77, 1.0) * star * twinkle;
        color *= 1.0 - 0.25 * smoothstep(0.3, 1.0, length(q));
        fragColor = vec4(color * uBackground, uBackground);
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
        float core = smoothstep(0.67, 0.42, r) * uPhase;
        vec3 color = uColor.rgb * alpha;
        alpha = max(alpha, core * 0.88 * uColor.a);
        fragColor = vec4(color, alpha);
    } else {
        float r = length(vLocal) * 2.0;
        float alpha = exp(-r * r * 5.0) * uColor.a;
        fragColor = vec4(uColor.rgb * alpha, alpha);
    }
}
