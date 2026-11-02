#version 450
// Full-res colour -> half-res luma (L0). One bilinear tap on the corner shared
// by a 2x2 quad lets the texture unit do the box filter for free.
layout(binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec2 invDst; float linearIn; } pc;
layout(location = 0) out float o;
void main() {
    vec3 c = textureLod(src, gl_FragCoord.xy * pc.invDst, 0.0).rgb;
    if (pc.linearIn > 0.5) c = sqrt(c); // sRGB view returns linear; match in gamma space
    // sqrt stretches dark tones: dark scenes otherwise leave the block matcher
    // with SADs made mostly of dither noise.
    o = sqrt(dot(c, vec3(0.299, 0.587, 0.114)));
}
