// Grass as blade models: a compute pass scatters blades on a world-fixed grid around the camera (so they never
// swim), reading height, materials and meadow from the terrain tiles; the vertex shader builds each curved,
// tapered blade and bends it in the wind; the pixel shader colours it from the blade texture.
#include "common.hlsli"
#include "gbuffer.hlsli"

static const float CELL = 0.25;          // metres per candidate cell
static const uint CANDIDATES = 4;        // blade candidates per cell near the camera
static const uint SEGMENTS = 5;
static const uint VERTS_PER_BLADE = SEGMENTS * 6;
static const uint GRID = K_TERRAIN_GRID;
static const uint VERTS = K_TERRAIN_GRID + 1;

struct GrassPush {
    float2 cornerRel;   // xz of the lookup grid corner, relative to the camera
    int2 cornerCell;    // world index of the corner cell (CELL metres)
    uint lookupSrv, lookupOffset, gridTiles, poolSrv;
    float tileSize, radius;
    uint bladesUav, argsUav;
    uint maxBlades, bladesSrv, bladeAlbedo, bladeDetail;
};
[[vk::push_constant]] ConstantBuffer<GrassPush> g_push : register(b1, space0);

[numthreads(1, 1, 1)]
void ResetMain() {
    g_rwBuffers[g_push.argsUav].Store4(0, uint4(VERTS_PER_BLADE, 0, 0, 0));
}

float rand01(uint h) { return float(h & 0xffffu) / 65535.0; }

uint4 tileVertex(uint slot, uint x, uint z) {
    return g_buffers[g_push.poolSrv].Load4(((slot * VERTS + z) * VERTS + x) * K_TERRAIN_VERTEX_BYTES);
}

[numthreads(8, 8, 1)]
void GenMain(uint3 id : SV_DispatchThreadID) {
    const float2 cellRel = g_push.cornerRel + (float2(id.xy) + 0.5) * CELL;
    const float d = length(cellRel);
    if (d > g_push.radius) return;
    const int2 cell = g_push.cornerCell + int2(id.xy);
    const float far = d / g_push.radius;
    const float keep = saturate(1.0 - far * far * 0.9);   // thinner with distance...
    const float widen = 1.0 + far * 2.5;                   // ...but wider, so the cover holds
    [unroll] for (uint k = 0; k < CANDIDATES; ++k) {
        const uint h = pcgHash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ (k * 83492791u));
        if (rand01(h) > keep) continue;
        const float2 local = (float2(id.xy) + float2(rand01(pcgHash(h + 1)), rand01(pcgHash(h + 2)))) * CELL;  // from the corner
        const int2 t = int2(floor(local / g_push.tileSize));
        if (any(t < 0) || any(t >= int(g_push.gridTiles))) continue;
        const int slot = asint(g_buffers[g_push.lookupSrv].Load(g_push.lookupOffset + uint(t.y * int(g_push.gridTiles) + t.x) * 4));
        if (slot < 0) continue;
        const float2 g = (local - float2(t) * g_push.tileSize) / g_push.tileSize * GRID;
        const uint2 g0 = min(uint2(g), uint2(GRID - 1, GRID - 1));
        const float2 f = g - float2(g0);
        const uint4 v00 = tileVertex(uint(slot), g0.x, g0.y), v10 = tileVertex(uint(slot), g0.x + 1, g0.y);
        const uint4 v01 = tileVertex(uint(slot), g0.x, g0.y + 1), v11 = tileVertex(uint(slot), g0.x + 1, g0.y + 1);
        const float hgt = lerp(lerp(asfloat(v00.x), asfloat(v10.x), f.x), lerp(asfloat(v01.x), asfloat(v11.x), f.x), f.y);
        const float4 m = lerp(lerp(unpackUnorm8x4(v00.z), unpackUnorm8x4(v10.z), f.x), lerp(unpackUnorm8x4(v01.z), unpackUnorm8x4(v11.z), f.x), f.y);
        const float4 e = lerp(lerp(unpackUnorm8x4(v00.w), unpackUnorm8x4(v10.w), f.x), lerp(unpackUnorm8x4(v01.w), unpackUnorm8x4(v11.w), f.x), f.y);
        const float meadow = e.x, wet = e.y;
        const float density = m.x * (0.35 + 0.65 * meadow) * (1.0 - wet);
        if (rand01(pcgHash(h + 3)) > density) continue;

        const float3 base = float3(g_push.cornerRel.x + local.x, (hgt - g_frame.cameraPosHigh.y) - g_frame.cameraPosLow.y, g_push.cornerRel.y + local.y);
        // Fire burnt the grass away here: none inside, short and charred at the edge.
        const float burnt = scorchAt(base);
        if (burnt > 0.85) continue;
        // Shorter toward the edge of the radius, so the field fades into the ground colour instead of ending in a line.
        const float height = lerp(0.22, 0.85, meadow) * (0.65 + 0.7 * rand01(pcgHash(h + 4))) * smoothstep(1.0, 0.55, far) * (1.0 - 0.8 * burnt);
        const float4 clip = mul(g_frame.viewProj, float4(base + float3(0, height * 0.5, 0), 1));
        const float margin = clip.w * 0.1 + height;
        if (clip.w <= 0 || abs(clip.x) > clip.w + margin || abs(clip.y) > clip.w + margin) continue;

        uint index;
        g_rwBuffers[g_push.argsUav].InterlockedAdd(4, 1u, index);
        if (index >= g_push.maxBlades) return;
        const float4 a = float4(base, height);
        const float4 b = float4(rand01(pcgHash(h + 5)) * 6.2831853, (0.010 + 0.008 * rand01(pcgHash(h + 6))) * widen,
                                0.3 + 0.7 * rand01(pcgHash(h + 7)),
                                lerp(rand01(pcgHash(h + 8)) * 0.6 + (1.0 - meadow) * 0.4, 2.0, burnt));  // w: dry 0..1, charred 1..2
        g_rwBuffers[g_push.bladesUav].Store4(index * 32, asuint(a));
        g_rwBuffers[g_push.bladesUav].Store4(index * 32 + 16, asuint(b));
    }
}

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 uvAo : TEXCOORD0;  // u across, v along, ambient occlusion
    float dry : TEXCOORD1;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const float4 a = asfloat(g_buffers[g_push.bladesSrv].Load4(iid * 32));
    const float4 b = asfloat(g_buffers[g_push.bladesSrv].Load4(iid * 32 + 16));
    const uint quad = vid / 6, corner = vid % 6;
    const uint2 offs[6] = {uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1)};
    const float side = float(offs[corner].x) * 2.0 - 1.0;
    const float t = float(quad + offs[corner].y) / SEGMENTS;

    const float2 facing = float2(cos(b.x), sin(b.x));
    const float3 across = float3(-facing.y, 0, facing.x);
    // Lean along the facing, more toward the tip; the wind pushes downwind in gusts that roll over the field.
    const float3 world = a.xyz + float3(g_frame.cameraWrap.x, 0, g_frame.cameraWrap.z);
    const float2 wind = g_frame.wind.xz;
    const float gust = 0.55 + 0.45 * sin(dot(world.xz, float2(0.21, 0.17)) - g_frame.time.z * 1.9 + dot(wind, world.xz) * 0.01);
    const float2 push = wind * (0.045 * gust) + facing * (b.z * 0.35);
    float3 p = a.xyz + float3(0, t * a.w, 0) + float3(push.x, 0, push.y) * (t * t * a.w);
    p.y -= dot(push, push) * t * t * a.w * 0.25;  // bending shortens the blade
    const float w = b.y * (1.0 - pow(t, 1.4));
    p += across * side * w * 0.5;

    const float3 along = normalize(float3(push.x * 2.0 * t, 1.0, push.y * 2.0 * t));
    float3 n = normalize(cross(across, along));
    n = normalize(lerp(n, float3(0, 1, 0), 0.45));  // soft, like a layer

    VSOut o;
    o.pos = mul(g_frame.viewProj, float4(p, 1));
    o.normal = n;
    o.uvAo = float3(side * 0.5 + 0.5, t, lerp(0.35, 1.0, t));
    o.dry = b.w;
    return o;
}

GBufferOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    const float4 c = g_tex2D[g_push.bladeAlbedo].Sample(s_linearClamp, i.uvAo.xy);
    const float4 d = g_tex2D[g_push.bladeDetail].Sample(s_linearClamp, i.uvAo.xy);
    float3 albedo = lerp(c.rgb, float3(0.17, 0.15, 0.07), saturate(i.dry));
    albedo = lerp(albedo, float3(0.025, 0.021, 0.018), saturate(i.dry - 1.0));  // charred at a burnt patch's edge
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    GBufferOut o;
    o.albedo = float4(albedo, i.uvAo.z);
    o.normal = float4(normalize(n + float3(0, 0.6, 0) * (front ? 0 : 1)), d.z);
    o.material = float4(0.45, 0.3, 0, 0);
    o.motion = 0;
    return o;
}

// After GenMain: never draw more blades than the buffer holds.
[numthreads(1, 1, 1)]
void ClampMain() {
    const uint count = g_rwBuffers[g_push.argsUav].Load(4);
    g_rwBuffers[g_push.argsUav].Store(4, min(count, g_push.maxBlades));
}
