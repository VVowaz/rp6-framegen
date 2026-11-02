#version 450
// Final full-resolution frame at time t, written straight into the target.
layout(binding = 0) uniform sampler2D prevColor;
layout(binding = 1) uniform sampler2D curColor;
layout(binding = 2) uniform sampler2D sel;

layout(push_constant) uniform PC {
    vec2 invFull;
    float t;
    int mode; // 0 = interpolate, 1 = show vectors, 2 = plain blend
    vec2 fallback; // match cost range over which we fade to a plain blend
} pc;

layout(location = 0) out vec4 o;

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

void main() {
    vec2 P = gl_FragCoord.xy;
    vec4 s = texelFetch(sel, ivec2(P * 0.5), 0);
    vec2 v = s.xy;
    if (pc.mode == 1) {
        float a = atan(v.y, v.x) / 6.2831853 + 0.5;
        o = vec4(hsv2rgb(vec3(a, clamp(length(v) / 32.0, 0.0, 1.0), 1.0)), 1.0);
        return;
    }
    if (pc.mode == 2) v = vec2(0.0);
    vec4 a = textureLod(prevColor, (P - pc.t * v) * pc.invFull, 0.0);
    vec4 b = textureLod(curColor, (P + (1.0 - pc.t) * v) * pc.invFull, 0.0);
    o = mix(a, b, pc.t);
    // Where even the best vector matches badly (noise, particles, occlusions)
    // a motion-compensated guess looks worse than a plain cross-fade.
    float f = smoothstep(pc.fallback.x, pc.fallback.y, s.z);
    if (f > 0.0) {
        vec4 blend = mix(textureLod(prevColor, P * pc.invFull, 0.0), textureLod(curColor, P * pc.invFull, 0.0), pc.t);
        o = mix(o, blend, f);
    }
}
