// Terrain tiles (CDLOD): heights, normals and ground materials come from the simulation, streamed into one pool.
// Odd vertices slide onto the parent grid near the end of a level's reach, so levels meet without cracks.
#include "common.hlsli"
#include "gbuffer.hlsli"

struct TerrainPush {
    uint nodesSrv;
    uint nodesOffset;
    uint poolSrv;
    uint pad;
    uint4 albedo;  // grass, dirt, rock, sand: rgb albedo, a height
    uint4 detail;  // xy tangent normal, z roughness, w occlusion
};
[[vk::push_constant]] ConstantBuffer<TerrainPush> g_push : register(b1, space0);

static const uint GRID = K_TERRAIN_GRID;
static const uint VERTS = K_TERRAIN_GRID + 1;

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float4 mats : TEXCOORD0;   // grass, dirt, rock, sand
    float2 extra : TEXCOORD1;  // meadow, wet
    float3 tex : TEXCOORD2;    // xz: world position modulo 256 m, y: world height
    float3 rel : TEXCOORD3;    // camera-relative position
};

uint4 loadVertex(uint slot, uint x, uint z) {
    return g_buffers[g_push.poolSrv].Load4(((slot * VERTS + z) * VERTS + x) * K_TERRAIN_VERTEX_BYTES);
}

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    TerrainNode node = g_buffers[g_push.nodesSrv].Load<TerrainNode>(g_push.nodesOffset + iid * 32);
    const uint vx = vid % VERTS, vz = vid / VERTS, slot = node.info.x;
    const float spacing = node.originSize.w / GRID;
    uint4 v = loadVertex(slot, vx, vz);
    float h = asfloat(v.x);
    float3 n = decodeOctNormal(v.y);
    float4 mats = unpackUnorm8x4(v.z), extra = unpackUnorm8x4(v.w);
    float2 local = float2(vx, vz) * spacing;

    float3 rel = float3(node.originSize.x + local.x, (h - g_frame.cameraPosHigh.y) - g_frame.cameraPosLow.y, node.originSize.z + local.y);
    const float morphStart = asfloat(node.info.z), morphEnd = asfloat(node.info.w);
    const float morph = saturate((length(rel) - morphStart) / max(morphEnd - morphStart, 1e-3));
    const uint2 odd = uint2(vx & 1u, vz & 1u);
    if (morph > 0 && any(odd != 0)) {
        // Slide toward the lower even neighbour; heights and the rest bilinear on this tile's own grid.
        const float2 t = float2(odd) * morph;
        uint4 vX = loadVertex(slot, vx - odd.x, vz), vZ = loadVertex(slot, vx, vz - odd.y), vXZ = loadVertex(slot, vx - odd.x, vz - odd.y);
        h = lerp(lerp(h, asfloat(vX.x), t.x), lerp(asfloat(vZ.x), asfloat(vXZ.x), t.x), t.y);
        n = normalize(lerp(lerp(n, decodeOctNormal(vX.y), t.x), lerp(decodeOctNormal(vZ.y), decodeOctNormal(vXZ.y), t.x), t.y));
        mats = lerp(lerp(mats, unpackUnorm8x4(vX.z), t.x), lerp(unpackUnorm8x4(vZ.z), unpackUnorm8x4(vXZ.z), t.x), t.y);
        extra = lerp(lerp(extra, unpackUnorm8x4(vX.w), t.x), lerp(unpackUnorm8x4(vZ.w), unpackUnorm8x4(vXZ.w), t.x), t.y);
        local -= t * spacing;
        rel = float3(node.originSize.x + local.x, (h - g_frame.cameraPosHigh.y) - g_frame.cameraPosLow.y, node.originSize.z + local.y);
    }

    VSOut o;
    o.pos = mul(g_frame.viewProj, float4(rel, 1));
    o.normal = n;
    o.mats = mats;
    o.extra = extra.xy;
    o.tex = float3(g_frame.cameraWrap.x + rel.x, h, g_frame.cameraWrap.z + rel.z);
    o.rel = rel;
    return o;
}

struct Layer {
    float3 albedo;
    float height;
    float3 normalTS;  // tangent space: x along +x, y along +z, z up
    float rough;
    float ao;
};

Layer sampleLayer(uint i, float2 uv) {
    float4 a = g_tex2D[g_push.albedo[i]].Sample(s_anisoWrap, uv);
    float4 d = g_tex2D[g_push.detail[i]].Sample(s_anisoWrap, uv);
    Layer l;
    l.albedo = a.rgb;
    l.height = a.a;
    float2 xy = d.xy * 2.0 - 1.0;
    l.normalTS = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    l.rough = d.z;
    l.ao = d.w;
    return l;
}

// Two scales of the same layer (4 m and 16 m, both divide 256 m) so the tiling doesn't show.
Layer sampleGround(uint i, float2 xz) {
    Layer a = sampleLayer(i, xz / 4.0), b = sampleLayer(i, xz / 16.0 + 0.37);
    Layer o;
    o.albedo = a.albedo * lerp(0.8, 1.2, b.height);
    o.height = a.height * 0.7 + b.height * 0.3;
    o.normalTS = normalize(float3(a.normalTS.xy + b.normalTS.xy * 0.12, a.normalTS.z));  // the big scale only tints
    o.rough = a.rough;
    o.ao = a.ao * lerp(0.85, 1.0, b.ao);
    return o;
}

GBufferOut PSMain(VSOut i) {
    float3 N = normalize(i.normal);
    float4 w = i.mats / max(dot(i.mats, 1.0), 1e-3);
    float2 xz = i.tex.xz;

    Layer L[4];
    L[0] = sampleGround(0, xz);
    L[1] = sampleGround(1, xz);
    L[3] = sampleGround(3, xz);
    // Rock is projected from three sides so cliffs don't stretch.
    float3 tw = pow(abs(N), 4.0);
    tw /= dot(tw, 1.0);
    Layer ry = sampleLayer(2, xz / 8.0), rx = sampleLayer(2, float2(xz.y, i.tex.y) / 8.0), rz = sampleLayer(2, float2(xz.x, i.tex.y) / 8.0);
    L[2].albedo = ry.albedo * tw.y + rx.albedo * tw.x + rz.albedo * tw.z;
    L[2].height = ry.height * tw.y + rx.height * tw.x + rz.height * tw.z;
    L[2].rough = ry.rough * tw.y + rx.rough * tw.x + rz.rough * tw.z;
    L[2].ao = ry.ao * tw.y + rx.ao * tw.x + rz.ao * tw.z;
    float3 nRock = normalize(float3(ry.normalTS.x, 0, ry.normalTS.y) * tw.y + float3(0, rx.normalTS.y, rx.normalTS.x) * tw.x +
                             float3(rz.normalTS.x, rz.normalTS.y, 0) * tw.z + N);
    L[2].normalTS = float3(0, 0, 1);

    // Height-aware blend: the higher material wins where two meet, so edges look worn, not smeared.
    float4 hw = w * (float4(L[0].height, L[1].height, L[2].height, L[3].height) + 0.25);
    hw = pow(max(hw, 1e-4), 4.0);
    hw /= dot(hw, 1.0);

    float3 albedo = 0, nTS = 0;
    float rough = 0, ao = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        albedo += L[k].albedo * hw[k];
        nTS += L[k].normalTS * hw[k];
        rough += L[k].rough * hw[k];
        ao += L[k].ao * hw[k];
    }
    // Planar detail normal on top of the tile normal (whiteout blend), rock's own triplanar normal mixed in.
    float3 detail = float3(nTS.x, 0, nTS.y);
    float3 n = normalize(N + detail * (1.0 - hw[2]));
    n = normalize(lerp(n, nRock, hw[2]));

    // Where grass grows, the ground takes the colour of the blades with distance (blades end at the grass radius).
    const float dist = length(i.rel);
    const float3 canopy = float3(0.075, 0.12, 0.03) * lerp(1.15, 0.85, i.extra.x);
    albedo = lerp(albedo, canopy, w.x * (0.55 + 0.45 * i.extra.x) * smoothstep(15.0, 55.0, dist));
    // Large patches of lusher and drier ground (a 256 m period stays exact far from the origin).
    albedo *= lerp(0.8, 1.15, g_tex2D[g_push.albedo[0]].SampleLevel(s_linearWrap, xz / 256.0, 3.0).a);
    const float wet = i.extra.y;
    albedo *= lerp(1.0, 0.55, wet);
    rough = lerp(rough, 0.35, wet);

    GBufferOut o;
    o.albedo = float4(albedo, ao);
    o.normal = float4(n, rough);
    o.material = float4(0, 0.5, wet, packMagic(MAGIC_HELD_EARTH, 0.5, false));  // the ground glows softer than what grows on it
    o.motion = 0;
    return o;
}
