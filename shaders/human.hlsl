// The human (step 6): MakeHuman's body converted into our format (assets/human), skinned on the GPU to its 163 bones
// in the idle pose, drawn into the G-buffer piece by piece (skin, eyes, brows, lashes, hair, clothes) with their
// textures. In the magic view it glows with the life magic it holds: densest in the heart and the brain (their cores
// glow through the body), flowing like blood: pulses out from the heart through the arteries with each beat, slower
// beads back through the veins.
#include "common.hlsli"
#include "gbuffer.hlsli"
#include "magic.hlsli"

struct HumanPush {
    uint vbSrv, bonesSrv, bonesOffset, vesselsSrv;
    uint albedo, normal, flags, vesselCount;  // textures (0: none), HumanMaterialFlags, how many vessel stretches
    float4 place;  // xyz: its feet relative to the camera, w: which way it faces (radians, +z turned toward +x)
    float4 info;   // x: how full its share of held magic is (0..1), y: the heartbeat (0..1), z: seconds (wrapped), w: roughness
    float4 heart;  // xyz: where its heart is (model space), w: specular
    float4 brain;  // xyz: the middle of its brain, w: last frame's bones (their offset, as bits)
    float4 prevPlace;  // last frame's feet relative to last frame's camera, which way it faced
    float4 breath;     // x: where it is in a breath (0..1: in, then out), y: its water magic (0..1 of its share)
};
[[vk::push_constant]] ConstantBuffer<HumanPush> g_push : register(b1, space0);

static const uint FLAG_ALPHA = 1, FLAG_SKIN = 4, FLAG_EYE = 8, FLAG_HAIR = 16, FLAG_CLOTH = 32;

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float3 rest : TEXCOORD1;  // the point on the body at rest (model space): the magic is laid out by it
    float3 rel : TEXCOORD2;   // relative to the camera
    float ao : TEXCOORD3;
    float4 curClip : TEXCOORD4;   // unjittered, this frame
    float4 prevClip : TEXCOORD5;  // last frame's pose and place
};

float3x4 boneMatrix(uint b, uint offset) {
    ByteAddressBuffer bones = g_buffers[g_push.bonesSrv];
    const uint at = offset + b * 48;
    return float3x4(asfloat(bones.Load4(at)), asfloat(bones.Load4(at + 16)), asfloat(bones.Load4(at + 32)));
}

// Vertex, 40 bytes: position, octahedral normal, octahedral tangent, uv, 4 bones (u8), 4 weights (unorm8), then the
// sky it sees (unorm8) and the tangent's handedness (bit 8).
VSOut VSMain(uint vid : SV_VertexID) {
    ByteAddressBuffer vb = g_buffers[g_push.vbSrv];
    const uint base = vid * 40;
    const float3 p = asfloat(vb.Load3(base));
    const float3 n = decodeOctNormal(vb.Load(base + 12));
    const float3 t = decodeOctNormal(vb.Load(base + 16));
    const float2 uv = asfloat(vb.Load2(base + 20));
    const uint bones = vb.Load(base + 28);
    const float4 w = unpackUnorm8x4(vb.Load(base + 32));
    const uint extra = vb.Load(base + 36);
    float3 sp = 0, sn = 0, st = 0, pp = 0;
    const uint prevOffset = asuint(g_push.brain.w);
    [unroll] for (uint i = 0; i < 4; ++i) {
        if (w[i] <= 0.0) continue;
        const uint b = (bones >> (8 * i)) & 255u;
        const float3x4 m = boneMatrix(b, g_push.bonesOffset);
        sp += w[i] * mul(m, float4(p, 1.0));
        sn += w[i] * mul((float3x3)m, n);
        st += w[i] * mul((float3x3)m, t);
        pp += w[i] * mul(boneMatrix(b, prevOffset), float4(p, 1.0));
    }
    // Turned to face its way (+z toward +x, as the trees), then stood on its spot.
    const float c = cos(g_push.place.w), s = sin(g_push.place.w);
    const float3 rel = g_push.place.xyz + float3(sp.x * c + sp.z * s, sp.y, -sp.x * s + sp.z * c);
    VSOut o;
    o.pos = mul(g_frame.viewProj, float4(rel, 1));
    o.normal = float3(sn.x * c + sn.z * s, sn.y, -sn.x * s + sn.z * c);
    o.tangent = float4(st.x * c + st.z * s, st.y, -st.x * s + st.z * c, (extra & 256u) ? -1.0 : 1.0);
    o.uv = uv;
    o.rest = p;
    o.rel = rel;
    o.ao = float(extra & 255u) / 255.0;
    // Its own motion on screen (DLSS): this frame unjittered against last frame's pose and place.
    o.curClip = o.pos;
    o.curClip.x -= 2.0 * g_frame.jitter.x / g_frame.jitter.z * o.pos.w;
    o.curClip.y += 2.0 * g_frame.jitter.y / g_frame.jitter.w * o.pos.w;
    const float pc = cos(g_push.prevPlace.w), ps = sin(g_push.prevPlace.w);
    const float3 prevRel = g_push.prevPlace.xyz + float3(pp.x * pc + pp.z * ps, pp.y, -pp.x * ps + pp.z * pc);
    o.prevClip = mul(g_frame.prevViewProj, float4(prevRel, 1));
    return o;
}

float3 srgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }

// ---------------------------------------------------------------- the magic view

// The life magic it holds at a point of its body (model space, kg/m3 at a full share): sim/human.cpp humanLifeMagicAt.
float lifeMagicAt(float3 p, float beat) {
    const float3 h = (p - g_push.heart.xyz) / K_HUMAN_HEART_RADIUS, b = (p - g_push.brain.xyz) / K_HUMAN_BRAIN_RADII;
    return K_MAGIC_HELD + (K_HUMAN_HEART_MAGIC - K_MAGIC_HELD) * exp(-dot(h, h)) * (0.8 + 0.4 * beat) +
           (K_HUMAN_BRAIN_MAGIC - K_MAGIC_HELD) * exp(-dot(b, b));
}

// The dense life magic of the heart and the brain glowing through the body along this view ray (from the surface
// point r inward, model space), the heart swelling with each beat.
float3 coresGlow(float3 r, float3 dir, float fill, float beat) {
    float sum = 0;
    const uint steps = 12;
    const float reach = 0.30, dt = reach / float(steps);
    [unroll] for (uint k = 0; k < steps; ++k) sum += max(lifeMagicAt(r + dir * ((float(k) + 0.5) * dt), beat) - K_MAGIC_HELD, 0.0) * dt;
    return (K_MAGIC_COLOR_LIFE * 1.6 + 0.12) * sum * fill * K_MAGIC_SURFACE_GLOW * 0.9;
}

// The heartbeat at a moment (sim/human.cpp humanHeartbeat): two close knocks a beat.
float heartbeatAt(float t) {
    const float ph = frac(t / K_HUMAN_HEARTBEAT_SECONDS);
    const float a = (ph - 0.0) / 0.055, b = (ph - 1.0) / 0.055, c = (ph - 0.30) / 0.055;
    return min(1.0, exp(-a * a) + exp(-b * b) + 0.65 * exp(-c * c));
}

// The blood vessels seen through the body along this view ray: each stretch glows where the ray passes close to it,
// fading with how deep in it lies; the arteries carry each heartbeat out as a pulse and beads of magic stream along
// them, the veins carry slower, dimmer beads back to the heart.
float3 vesselsGlow(float3 r, float3 dir, float fill) {
    if (g_push.vesselCount == 0) return 0;
    ByteAddressBuffer vs = g_buffers[g_push.vesselsSrv];
    const float t = g_push.info.z;
    float artery = 0, vein = 0;
    for (uint i = 0; i < g_push.vesselCount; ++i) {
        const float4 A = asfloat(vs.Load4(i * 32)), Bv = asfloat(vs.Load4(i * 32 + 16));
        const float3 a = A.xyz, v = Bv.xyz - A.xyz;
        const float vv = max(dot(v, v), 1e-8);
        // Closest approach of the ray (beyond the surface) and the stretch.
        float s = saturate(dot(r - a, v) / vv);
        float along = max(dot(a + v * s - r, dir), 0.0);
        s = saturate(dot(r + dir * along - a, v) / vv);
        along = max(dot(a + v * s - r, dir), 0.0);
        const float3 gap = r + dir * along - (a + v * s);
        const float d2 = dot(gap, gap);
        const bool isArtery = Bv.w > 0.0;
        const float width = isArtery ? 0.0075 : 0.006;
        if (d2 > 9.0 * width * width) continue;
        const float f = lerp(A.w, abs(Bv.w), s);  // metres from the heart along the vessels
        const float seen = exp(-d2 / (width * width)) * exp(-along / 0.07);
        if (isArtery) {
            const float pulse = heartbeatAt(t - f / K_HUMAN_PULSE_SPEED);
            const float beads = pow(0.5 + 0.5 * cos(6.2831853 * (f - t * K_HUMAN_BLOOD_SPEED) / 0.08), 6.0);
            artery += seen * (0.35 + 1.3 * pulse + 0.45 * beads);
        } else {
            const float beads = pow(0.5 + 0.5 * cos(6.2831853 * (f + t * K_HUMAN_BLOOD_SPEED * 0.5) / 0.11), 6.0);
            vein += seen * (0.22 + 0.35 * beads);
        }
    }
    const float3 col = K_MAGIC_COLOR_LIFE;
    return ((col * 2.2 + 0.10) * artery + (col * 1.2 + float3(0.0, 0.03, 0.06)) * vein) * fill * K_MAGIC_SURFACE_GLOW;
}

// Air magic through its airways with each breath (the user): drawn in from the nose down to the lungs while it breathes
// in, out again after, in beads that move with the air; the lungs either side of the heart fill with it.
float3 breathGlow(float3 r, float3 dir, float fill) {
    const uint airways = g_push.flags >> 16;
    if (airways == 0) return 0;
    ByteAddressBuffer vs = g_buffers[g_push.vesselsSrv];
    const float phase = g_push.breath.x;
    const float full = 0.5 - 0.5 * cos(6.2831853 * phase);  // how much air is in it
    const float moving = abs(sin(6.2831853 * phase));        // how fast it moves now
    float air = 0;
    for (uint i = 0; i < airways; ++i) {
        const uint at = (g_push.vesselCount + i) * 32;
        const float4 A = asfloat(vs.Load4(at)), Bv = asfloat(vs.Load4(at + 16));
        const float3 a = A.xyz, v = Bv.xyz - A.xyz;
        const float vv = max(dot(v, v), 1e-8);
        float s = saturate(dot(r - a, v) / vv);
        float along = max(dot(a + v * s - r, dir), 0.0);
        s = saturate(dot(r + dir * along - a, v) / vv);
        along = max(dot(a + v * s - r, dir), 0.0);
        const float3 gap = r + dir * along - (a + v * s);
        const float d2 = dot(gap, gap);
        const float f = lerp(A.w, Bv.w, s);  // m from the nose
        const float width = lerp(0.009, 0.004, saturate((f - 0.25) / 0.15));  // the windpipe, then the bronchi
        if (d2 > 9.0 * width * width) continue;
        const float seen = exp(-d2 / (width * width)) * exp(-along / 0.08);
        // Beads riding the air: in as it fills, back out as it empties.
        const float beads = pow(0.5 + 0.5 * cos(6.2831853 * (f - 0.25 * full * K_HUMAN_AIR_SPEED) / 0.05), 4.0);
        air += seen * (0.3 + 1.1 * beads * (0.3 + 0.7 * moving));
    }
    // The lungs: through each along the ray, fuller as it breathes in.
    float lungs = 0;
    const uint first = g_push.vesselCount + airways;
    [unroll] for (uint s = 0; s < 2; ++s) {
        const float3 c = asfloat(vs.Load3((first + s) * 32));
        [unroll] for (uint k = 0; k < 8; ++k) {
            const float3 q = (r + dir * ((float(k) + 0.5) * 0.035) - c) / (K_HUMAN_LUNG_RADII * (0.92 + 0.08 * full));
            lungs += exp(-dot(q, q) * 1.5) * 0.035;
        }
    }
    return K_MAGIC_COLOR_AIR * (2.2 * air + 18.0 * lungs * (0.3 + 0.7 * full)) * fill * K_MAGIC_SURFACE_GLOW;
}

// ---------------------------------------------------------------- the surface

GBufferOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    float4 a = float4(0.5, 0.5, 0.5, 1.0);
    if (g_push.albedo != 0) {
        Texture2D<float4> tex = g_tex2D[g_push.albedo];
        a = tex.Sample(s_anisoWrap, i.uv);
        if (g_push.flags & FLAG_ALPHA) {
            // Mips average the alpha away; push it back up so far hair doesn't thin out.
            const float lod = tex.CalculateLevelOfDetail(s_anisoWrap, i.uv);
            if (a.a * (1.0 + 0.35 * max(lod, 0.0)) < 0.5) discard;
        }
        a.rgb = srgbToLinear(a.rgb);
    }
    float3 N = normalize(i.normal) * (front ? 1.0 : -1.0);
    if (g_push.normal != 0) {
        const float3 T = normalize(i.tangent.xyz - N * dot(i.tangent.xyz, N));
        const float3 B = cross(N, T) * i.tangent.w;
        const float3 m = g_tex2D[g_push.normal].Sample(s_anisoWrap, i.uv).xyz * 2.0 - 1.0;
        N = normalize(T * m.x + B * m.y + N * max(m.z, 0.2));
    }
    float rough = g_push.info.w, specular = g_push.heart.w, translucency = 0.0, subsurface = 0.0;
    if (g_push.flags & FLAG_SKIN) {
        translucency = 0.3;
        subsurface = 1.0;
        rough *= 0.9 + 0.2 * latticeNoise3(i.rest * 1300.0);  // pores in its sheen
    }
    if (g_push.flags & FLAG_HAIR) translucency = 0.35;

    GBufferOut o;
    if (magicView()) {
        // Its life magic, fuller over the heart and the brain, the cores and the vessels glowing through it.
        const float fill = g_push.info.x, beat = g_push.info.y;
        const float c = cos(g_push.place.w), s = sin(g_push.place.w);
        const float3 v = normalize(i.rel);  // the view ray, world; into the model's frame
        const float3 dir = float3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
        const float local = min(lifeMagicAt(i.rest, beat) / K_MAGIC_HELD, 2.5);
        // Its water magic (the user: it is mostly water) streams through it too, slower, in its own blue.
        const float waterFlow = i.rest.y * 0.8 + length(i.rest.xz) * 2.0;
        const float waterThreads = pow(0.5 + 0.5 * cos(i.rest.x * 40.0 + i.rest.z * 25.0 + i.rest.y * 6.0), 2.0);
        const float3 glow = heldMagicGlow(MAGIC_HELD_LIFE, fill * local * 0.6, length(i.rest - g_push.heart.xyz) * 1.15, 1.0) +
                            heldMagicGlow(MAGIC_HELD_WATER, g_push.breath.y * K_HUMAN_WATER / K_MAGIC_HELD * 0.3, waterFlow, waterThreads * 0.7) +
                            coresGlow(i.rest, dir, fill, beat) + vesselsGlow(i.rest, dir, fill) + breathGlow(i.rest, dir, 1.0);
        o.albedo = magicViewAlbedo(a.rgb, glow);
    } else {
        o.albedo = float4(a.rgb, i.ao);
    }
    o.normal = float4(N, rough);
    o.material = float4(translucency, specular, subsurface, packMagic(MAGIC_HELD_LIFE, g_push.info.x, true));
    float2 motion = 0;
    if (g_frame.cameraDelta.w > 0 && i.prevClip.w > 1e-6) {
        const float2 cur = i.curClip.xy / i.curClip.w * float2(0.5, -0.5) + 0.5;
        const float2 prev = i.prevClip.xy / i.prevClip.w * float2(0.5, -0.5) + 0.5;
        motion = prev - cur;
    }
    o.motion = motion;  // its own motion (it runs, swims, flies)
    return o;
}
