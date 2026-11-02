#version 450
#extension GL_QCOM_image_processing : require
// Per-pixel vector selection at half resolution for intermediate time t.
// Candidates come from the forward and backward block fields around the pixel
// (projected to time t); the one whose two ends match best in luma wins.
// Duplicate candidates are skipped and a near-perfect match ends the search,
// so waves inside smoothly moving regions do a single evaluation.
// A candidate's cost is ONE hardware block-match SAD over 4x4 luma texels at
// the two ends of the vector (positions rounded to the L0 grid): the texture
// unit compares 16 texel pairs per instruction instead of 8 bilinear taps.
layout(binding = 0) uniform sampler2D prevBM;
layout(binding = 1) uniform sampler2D curBM;
layout(binding = 2) uniform sampler2D fieldF;
layout(binding = 3) uniform sampler2D fieldB;

layout(push_constant) uniform PC {
    vec2 fullSize;
    ivec2 grid;
    float t;
    int pad;
    ivec2 lumaSize;  // L0 size
} pc;

layout(location = 0) out vec4 o;

const float BLOCK = 16.0;     // L0 block size in full-res pixels
const float SAME = 0.25;      // candidates closer than this (px) count as duplicates
const float GOOD = 0.05;      // 16-texel SAD considered a perfect match

vec2 fwd(vec2 P, ivec2 off) {
    return texelFetch(fieldF, clamp(ivec2(P / BLOCK) + off, ivec2(0), pc.grid - 1), 0).xy;
}
// Backward vectors converted to the forward convention (prev -> cur).
vec2 bwd(vec2 P, ivec2 off) {
    return -texelFetch(fieldB, clamp(ivec2(P / BLOCK) + off, ivec2(0), pc.grid - 1), 0).xy;
}

float cost(vec2 P, vec2 v) {
    // Block of 4x4 L0 texels centred on each end of the vector.
    vec2 a = (P - pc.t * v) * 0.5 - 1.5;
    ivec2 maxO = pc.lumaSize - 4;
    // Round one end and step by the rounded vector: the two blocks are then
    // misaligned by at most half a texel, not a full one.
    ivec2 ia = ivec2(floor(a + 0.5));
    ivec2 ib = ia + ivec2(floor(v * 0.5 + 0.5));
    ivec2 ca = clamp(ia, ivec2(0), maxO), cb = clamp(ib, ivec2(0), maxO);
    float e = textureBlockMatchSADQCOM(prevBM, uvec2(ca), curBM, uvec2(cb), uvec2(4)).x;
    return any(notEqual(ia, ca)) || any(notEqual(ib, cb)) ? e + 0.2 : e;
}

vec2 best;
float bestCost;

void try_(vec2 P, vec2 v) {
    if (bestCost < GOOD || all(lessThan(abs(v - best), vec2(SAME)))) return;
    float c = cost(P, v);
    if (c < bestCost) { bestCost = c; best = v; }
}

void main() {
    vec2 P = gl_FragCoord.xy * 2.0;
    float t = pc.t;

    vec2 pf = P - t * fwd(P, ivec2(0));         // where this pixel sat in prev
    vec2 pb = P + (1.0 - t) * bwd(P, ivec2(0)); // where this pixel sits in cur

    best = fwd(pf, ivec2(0));
    bestCost = cost(P, best);
    try_(P, bwd(pb, ivec2(0)));
    try_(P, vec2(0.0));
    try_(P, fwd(pf, ivec2(1, 0)));
    try_(P, fwd(pf, ivec2(-1, 0)));
    try_(P, fwd(pf, ivec2(0, 1)));
    try_(P, fwd(pf, ivec2(0, -1)));
    try_(P, bwd(pb, ivec2(1, 0)));
    try_(P, bwd(pb, ivec2(-1, 0)));
    try_(P, bwd(pb, ivec2(0, 1)));
    try_(P, bwd(pb, ivec2(0, -1)));
    o = vec4(best, bestCost, 0.0);
}
