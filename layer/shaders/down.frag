#version 450
// Pyramid level k -> k+1, same bilinear 2x2 trick.
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec2 invDst; } pc;
layout(location = 0) out float o;
void main() {
    o = textureLod(src, gl_FragCoord.xy * pc.invDst, 0.0).r;
}
