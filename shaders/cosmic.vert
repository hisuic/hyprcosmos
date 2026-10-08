#version 300 es
precision highp float;

layout(location = 0) in vec2 aPosition;
uniform mat3 uProjection;
uniform vec2 uResolution;
uniform vec2 uCenter;
uniform vec2 uSize;
uniform float uAngle;
uniform float uStretch;
uniform float uTwist;
uniform float uBend;
uniform vec4 uCrop;
out vec2 vUV;
out vec2 vLocal;
out vec2 vScreen;

void main() {
    vec2 local = aPosition - 0.5;
    vec2 normalized = local * 2.0;
    float winding = uTwist * min(1.0, length(normalized));
    float wc = cos(winding);
    float ws = sin(winding);
    normalized = mat2(wc, ws, -ws, wc) * normalized;
    vec2 offset = normalized * 0.5 * uSize;
    offset *= vec2(uStretch, 1.0 / max(uStretch, 0.08));
    // A tessellated quad bends the whole application image without resizing it.
    offset.y += sin(local.x * 6.2831853) * uBend * uSize.y;
    float turn = uAngle;
    float c = cos(turn);
    float s = sin(turn);
    offset = mat2(c, s, -s, c) * offset;
    vec2 pixel = uCenter + offset;
    vec3 projected = uProjection * vec3(pixel / uResolution, 1.0);
    gl_Position = vec4(projected.xy, 0.0, 1.0);
    vUV = uCrop.xy + aPosition * uCrop.zw;
    vLocal = local;
    vScreen = pixel / uResolution;
}
