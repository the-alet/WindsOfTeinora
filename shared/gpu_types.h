// Structs shared by C++ and HLSL: one definition, included by both, so layouts can't drift.
// Only float4 / float4x4 / uint4 members (16-byte rows) so C++ and HLSL packing agree.
#ifndef TAELIA_GPU_TYPES_H
#define TAELIA_GPU_TYPES_H

#ifdef __cplusplus
#include <cstdint>
namespace taelia::gpu {
struct float4 { float x, y, z, w; };
struct uint4 { uint32_t x, y, z, w; };
struct float4x4 { float m[16]; };  // row-major in memory; HLSL reads it with row_major
#define TAELIA_ROW_MAJOR
#else
#define TAELIA_ROW_MAJOR row_major
#endif

// Frame constants (b0). Budget: FRAME_CONSTANTS_BYTES (8 KB); keep growing at the end.
struct FrameConstants {
    TAELIA_ROW_MAJOR float4x4 viewProj;       // camera-relative world -> clip (reverse-Z, infinite far)
    TAELIA_ROW_MAJOR float4x4 invViewProj;    // clip -> camera-relative world
    TAELIA_ROW_MAJOR float4x4 view;           // camera-relative world -> view
    float4 cameraPosHigh;   // xyz: camera position, high part (double split), w: unused
    float4 cameraPosLow;    // xyz: low part, so world = high + low + relative
    float4 cameraForward;   // xyz, w: tan(fovY/2)
    float4 cameraRight;     // xyz, w: aspect
    float4 cameraUp;        // xyz, w: near plane
    float4 viewport;        // width, height, 1/width, 1/height
    float4 gaze;            // xy: gaze point in uv (0..1, y down), z: focus radius, w: blur max (uv of height)
    float4 time;            // x: simulated seconds (mod a day), y: hour of day, z: real seconds, w: time speed
    float4 sunDir;          // xyz: toward the main light, w: unused until the sky arrives
    float4 sunColor;        // rgb: radiance, w: unused
    float4 ambient;         // rgb: ambient light
    float4 wind;            // xyz: m/s, w: unused
    uint4 flags;            // x: magic view, y: frame number, z: debug view, w: ray tracing (1 if the TLAS is valid)
    float4 cameraWrap;      // xz: camera position modulo 256 m, yw: modulo 4096 m (texturing and patterns far from the origin)
    TAELIA_ROW_MAJOR float4x4 prevViewProj;  // last frame: camera-relative (to last frame's camera) world -> clip, no jitter
    float4 cameraDelta;     // xyz: this camera position minus last frame's, w: 1 if last frame is valid
    float4 jitter;          // xy: this frame's jitter in render pixels, zw: render size
    // The sky (sim/sky.h), local frame: x east, y up, z north.
    float4 suns[2];         // xyz: toward each sun, w: how much of it the planet hides (0..1)
    float4 sunLight[2];     // rgb: its light above the atmosphere (colour x intensity), w: apparent radius (radians)
    float4 planet;          // xyz: toward the planet, w: apparent radius (radians)
    float4 planetLit[2];    // xyz: from the planet toward each sun (its phase), w: unused
    float4 planetPose;      // x: spin, y: ring opening, z: axis angle on the sky (radians), w: albedo
    float4 worldNorth;      // xyz: the world's axis (the sky turns about it), w: camera altitude above sea level (m)
    TAELIA_ROW_MAJOR float4x4 starRotation;  // inertial -> local (stars, the galactic band); 3x3 in the top left
    float4 galacticPole;    // xyz: the invented galaxy's pole (inertial), w: star brightness scale
    float4 nightLight;      // rgb: the planet's diffuse light on the ground at night (from its direction), w: airglow
    float4 clouds;          // xy: how far the wind carried the clouds (m, x and z), z: cover 0..1, w: map texel size (m)
    float4 cloudMap;        // xy: world xz of the cloud map's corner relative to the camera, z: map size (texels), w: cloud thickness for the day's cover (m), used past the map's edge
    float4 exposure;        // x: exposure multiplier, yzw: white balance (the eye half adapts to the light's colour)
    uint4 atmosphere;       // bindless: x transmittance LUT, y multiple scattering LUT, z sky view LUT, w aerial perspective volume
    uint4 atmosphere2;      // bindless: x sky irradiance, y cloud shadow map, z cloud map, w clouds (colour + transmittance)
    uint4 scene;            // x: TLAS bindless index, y: sky texture, z, w: unused
<<<<<<< Updated upstream
=======
    float4 magicWindow[2];  // the magic volumes (L0, L1): xyz the window's min corner relative to the camera (m), w: cell width (m)
    float4 magicInfo;       // x: L0 cell height, y: L1 cell height (m), z: view reach (m), w: 1 if the volumes hold the field
    uint4 magicVolume;      // bindless: x L0 volume, y L1 volume (rgb: colour x density, a: total density, kg/m3)
    float4 fire;            // the campfire: xyz its bed relative to the camera (m), w: how strongly it burns (0: out)
    float4 fireInfo;        // x: seconds (wrapped hourly), yz: the wind 2 m up (m/s, x and z), w: unused
    float4 flames[3];       // fire condensed elsewhere: xyz relative to the camera (m), w strength like the campfire's (0: none)
    float4 denseLo;         // magic past the dew point: its box relative to the camera (xyz), w 1 if there is any
    float4 denseHi;
    float4 weather;         // x: rain here (0..1), y: storm (0..1), z: lightning flash (0..1), w: unused
    float4 bolt[2];         // the last strike: [0] xyz where it hit (relative to the camera), w seed bits; [1] xyz the cloud base above it, w its age (s, < 0: none)
    float4 steam[2];        // steam where fire past the dew point meets water: xyz its base on the surface relative to the camera (m), w strength (0: none)
    float4 scorch[8];       // grass burnt away: xyz the circle's centre on the ground relative to the camera (m), w its radius (0: none)
>>>>>>> Stashed changes
};


// One terrain tile to draw (read by terrain.hlsl from the upload ring). 32 bytes.
struct TerrainNode {
    float4 originSize;  // xz: tile corner relative to the camera, y: unused, w: tile size in metres
    uint4 info;         // x: tile slot in the pool, y: level, z: morph start (float bits), w: morph end (float bits)
};

#ifdef __cplusplus
static_assert(sizeof(FrameConstants) % 16 == 0, "frame constants are 16-byte rows");
static_assert(sizeof(FrameConstants) <= 8192, "frame constants budget is 8 KB");
static_assert(sizeof(TerrainNode) == 32, "terrain.hlsl reads 32-byte nodes");
}  // namespace taelia::gpu
#endif

#undef TAELIA_ROW_MAJOR
#endif
