#include "Common.hlsli"

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0; // clip-space position, for reconstructing view rays
	float2 Uv : TEXCOORD1;
};

// One triangle covering the screen, at depth 0: the far plane with reversed depth.
Varyings VSMain(uint Vertex : SV_VertexID)
{
	const float2 Corner = float2((Vertex << 1) & 2, Vertex & 2);

	Varyings Output;
	Output.Screen = Corner * 2.0 - 1.0;
	Output.Clip = float4(Output.Screen, 0.0, 1.0);
	Output.Uv = float2(Corner.x, 1.0 - Corner.y);
	return Output;
}
