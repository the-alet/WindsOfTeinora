// The names the passes before step 3 used, now backed by the atmosphere (atmosphere.hlsli).
#ifndef TAELIA_SKY_HLSLI
#define TAELIA_SKY_HLSLI

#include "atmosphere.hlsli"

float3 skyColor(float3 dir) { return skyRadiance(dir); }
float3 skyAmbient(float3 n) { return skyIrradiance(n); }
float3 applyHaze(float3 color, float3 rel) { return aerialPerspective(color, rel); }

#endif
