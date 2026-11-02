#version 450
#extension GL_QCOM_image_processing : require
#extension GL_EXT_control_flow_attributes : require
// Block motion estimation on one pyramid level, forward and backward at once
// (two render targets). One fragment = one 8x8 block; matching cost is the
// texture unit's hardware block-match SAD (ir3 `sad` on the TPL1).
//
// Everything is unrolled so the SAD requests for a whole batch are issued
// back to back and their latency overlaps instead of stalling per call.
// Candidates follow 3DRS (TV MEMC chips): parent-level vectors, last frame's
// vector and zero; then a 3x3 (or RADIUS) full search around the winner,
// whose samples also give the sub-pixel parabola fit for free.
layout(constant_id = 0) const int RADIUS = 1;
layout(constant_id = 1) const int BS = 8;

layout(binding = 0) uniform sampler2D prevBM;
layout(binding = 1) uniform sampler2D curBM;
layout(binding = 2) uniform sampler2D parentF;
layout(binding = 3) uniform sampler2D parentB;
layout(binding = 4) uniform sampler2D tempF;
layout(binding = 5) uniform sampler2D tempB;

layout(push_constant) uniform PC {
    ivec2 levelSize;
    ivec2 parentGrid;
    ivec2 tempGrid;
    float scale;   // full-res pixels per level pixel
    float lambda;  // penalty per level pixel of deviation from the parent vector
    int parentShift; // log2(parent block / child block, in full-res pixels)
    int flags;     // 1 = has parent, 2 = has temporal, 4 = sub-pixel refine
} pc;

layout(location = 0) out vec4 outF;
layout(location = 1) out vec4 outB;

// Candidates: zero, parent, [parent neighbours,] temporal (NC = 6 or 3).
layout(constant_id = 2) const int NC = 6;
const int SIDE = 2 * RADIUS + 1;

ivec2 o, lo, hi;

float sadF(ivec2 c) { return textureBlockMatchSADQCOM(prevBM, uvec2(o), curBM, uvec2(o + c), uvec2(BS)).x; }
float sadB(ivec2 c) { return textureBlockMatchSADQCOM(curBM, uvec2(o), prevBM, uvec2(o + c), uvec2(BS)).x; }

ivec2 vecAt(sampler2D tex, ivec2 pg) {
    return ivec2(round(texelFetch(tex, clamp(pg, ivec2(0), pc.parentGrid - 1), 0).xy / pc.scale));
}

void candidates(sampler2D parent, sampler2D temp, ivec2 g, out ivec2 cand[NC], out vec2 pred) {
    bool hasParent = (pc.flags & 1) != 0;
    bool hasTemp = (pc.flags & 2) != 0;
    ivec2 pg = g >> pc.parentShift;
    ivec2 d = ivec2((g.x & 1) != 0 ? 1 : -1, (g.y & 1) != 0 ? 1 : -1);
    pred = hasParent ? texelFetch(parent, clamp(pg, ivec2(0), pc.parentGrid - 1), 0).xy / pc.scale : vec2(0.0);
    ivec2 tg = clamp(ivec2(vec2(g) * pc.scale * float(BS) / 16.0), ivec2(0), pc.tempGrid - 1);
    cand[0] = ivec2(0);
    cand[1] = ivec2(round(pred));
    if (NC == 6) {
        cand[2] = hasParent ? vecAt(parent, pg + ivec2(d.x, 0)) : ivec2(0);
        cand[3] = hasParent ? vecAt(parent, pg + ivec2(0, d.y)) : ivec2(0);
        cand[4] = hasParent ? vecAt(parent, pg + d) : ivec2(0);
    }
    cand[NC - 1] = hasTemp ? ivec2(round(texelFetch(temp, tg, 0).xy / pc.scale)) : ivec2(0);
    [[unroll]] for (int i = 0; i < NC; i++) cand[i] = clamp(cand[i], lo, hi);
}

// lambda is tuned for 8x8 SAD sums; scale with block area so 4x4 levels are not over-regularised.
float penalty(ivec2 c, vec2 pred) {
    return pc.lambda * float(BS * BS) / 64.0 * dot(abs(vec2(c) - pred), vec2(1.0));
}

// Parabola through three samples, vertex offset in [-0.5, 0.5].
float parabola(float l, float c, float r) {
    float d = l - 2.0 * c + r;
    return d > 1e-5 ? clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
}

void main() {
    ivec2 g = ivec2(gl_FragCoord.xy);
    ivec2 maxOrigin = pc.levelSize - BS;
    o = min(g * BS, maxOrigin);
    lo = -o;
    hi = maxOrigin - o;

    ivec2 cf[NC], cb[NC];
    vec2 pf, pb;
    candidates(parentF, tempF, g, cf, pf);
    candidates(parentB, tempB, g, cb, pb);

    // Batch 1: all candidate SADs, both directions.
    float sf[NC], sb[NC];
    [[unroll]] for (int i = 0; i < NC; i++) { sf[i] = sadF(cf[i]); sb[i] = sadB(cb[i]); }
    ivec2 bf = cf[0], bb = cb[0];
    float bestF = sf[0] + penalty(cf[0], pf), bestB = sb[0] + penalty(cb[0], pb);
    [[unroll]] for (int i = 1; i < NC; i++) {
        float f = sf[i] + penalty(cf[i], pf), b = sb[i] + penalty(cb[i], pb);
        if (f < bestF) { bestF = f; bf = cf[i]; }
        if (b < bestB) { bestB = b; bb = cb[i]; }
    }

    // Batch 2: full search window around the winners. Centres are pulled in so
    // the whole window stays inside the image (only matters at the borders).
    bf = clamp(bf, min(lo + RADIUS, hi), max(hi - RADIUS, lo));
    bb = clamp(bb, min(lo + RADIUS, hi), max(hi - RADIUS, lo));
    vec2 vF, vB;
    float rawF, rawB;
    if (RADIUS == 1) {
        float wf[SIDE * SIDE], wb[SIDE * SIDE];
        [[unroll]] for (int y = 0; y < SIDE; y++)
        [[unroll]] for (int x = 0; x < SIDE; x++) {
            ivec2 d = ivec2(x - RADIUS, y - RADIUS);
            wf[y * SIDE + x] = sadF(clamp(bf + d, lo, hi));
            wb[y * SIDE + x] = sadB(clamp(bb + d, lo, hi));
        }
        int kf = 0, kb = 0;
        float mf = 1e9, mb = 1e9;
        [[unroll]] for (int i = 0; i < SIDE * SIDE; i++) {
            ivec2 d = ivec2(i % SIDE - RADIUS, i / SIDE - RADIUS);
            float f = wf[i] + penalty(bf + d, pf), b = wb[i] + penalty(bb + d, pb);
            if (f < mf) { mf = f; kf = i; }
            if (b < mb) { mb = b; kb = i; }
        }
        ivec2 xyF = ivec2(kf % SIDE, kf / SIDE), xyB = ivec2(kb % SIDE, kb / SIDE);
        vec2 subF = vec2(0.0), subB = vec2(0.0);
        if ((pc.flags & 4) != 0) {
            if (xyF.x > 0 && xyF.x < SIDE - 1) subF.x = parabola(wf[kf - 1], wf[kf], wf[kf + 1]);
            if (xyF.y > 0 && xyF.y < SIDE - 1) subF.y = parabola(wf[kf - SIDE], wf[kf], wf[kf + SIDE]);
            if (xyB.x > 0 && xyB.x < SIDE - 1) subB.x = parabola(wb[kb - 1], wb[kb], wb[kb + 1]);
            if (xyB.y > 0 && xyB.y < SIDE - 1) subB.y = parabola(wb[kb - SIDE], wb[kb], wb[kb + SIDE]);
        }
        vF = vec2(clamp(bf + xyF - RADIUS, lo, hi)) + subF;
        vB = vec2(clamp(bb + xyB - RADIUS, lo, hi)) + subB;
        rawF = wf[kf];
        rawB = wb[kb];
    } else {
        // Large windows: running arg-min, no arrays (dynamic indexing spills to private memory).
        float mf = 1e9, mb = 1e9;
        ivec2 kF = bf, kB = bb;
        rawF = rawB = 0.0;
        [[unroll]] for (int y = -RADIUS; y <= RADIUS; y++)
        [[unroll]] for (int x = -RADIUS; x <= RADIUS; x++) {
            ivec2 c0 = clamp(bf + ivec2(x, y), lo, hi), c1 = clamp(bb + ivec2(x, y), lo, hi);
            float s0 = sadF(c0), s1 = sadB(c1);
            float f = s0 + penalty(c0, pf), b = s1 + penalty(c1, pb);
            if (f < mf) { mf = f; kF = c0; rawF = s0; }
            if (b < mb) { mb = b; kB = c1; rawB = s1; }
        }
        vF = vec2(kF);
        vB = vec2(kB);
    }
    outF = vec4(vF * pc.scale, rawF / float(BS * BS), 0.0);
    outB = vec4(vB * pc.scale, rawB / float(BS * BS), 0.0);
}
