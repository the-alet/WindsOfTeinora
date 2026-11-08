// Flames over what burns, each a volume: Flame.hlsli traces the eye's ray through it. The box around each
// is drawn from inside, so that the eye can stand in one, and the scene's depth says where the ray stops.
// It adds to what is behind it, as light does.
#include "Common.hlsli"
#include "Flame.hlsli"

struct Varyings
{
	float4 Clip : SV_Position;
	float3 Relative : TEXCOORD0; // this point of the box, relative to the camera
	float3 Foot : TEXCOORD1;     // the flame's foot, likewise
	float Height : TEXCOORD2;
	float Seed : TEXCOORD3;      // so that no two fires move alike
	float ViewDepth : TEXCOORD4;
	float Breadth : TEXCOORD5;
};

Varyings VSMain(uint Vertex : SV_VertexID, uint Instance : SV_InstanceID)
{
	// A box's twelve triangles, as corners of the unit cube.
	static const uint Corners[36] = {
		0, 2, 1, 1, 2, 3,  4, 5, 6, 5, 7, 6,  0, 1, 4, 1, 5, 4,
		2, 6, 3, 3, 6, 7,  0, 4, 2, 2, 4, 6,  1, 3, 5, 3, 7, 5 };
	const uint Corner = Corners[Vertex];
	const float3 Unit = float3(Corner & 1u, (Corner >> 1) & 1u, (Corner >> 2) & 1u);
	const float Height = HotPoints[Instance].w;

	Varyings Output;
	Output.Height = Height;
	Output.Breadth = HotShapes[Instance].x;
	Output.Seed = Instance * 37.0;
	Output.Foot = HotPoints[Instance].xyz - float3(0.0, 0.0, Height * 0.33);
	Output.Relative = Output.Foot + float3((Unit.xy * 2.0 - 1.0) * Height * GetFlameWidth(Output.Breadth), Unit.z * Height * FlameReach);
	Output.Clip = Height > 0.0 ? mul(ViewProjection, float4(Output.Relative, 1.0)) : float4(2.0, 2.0, 0.0, 1.0); // off the screen, for what only glows
	Output.ViewDepth = Output.Clip.w;
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	// The ray from the eye, as far as the nearest thing the scene has drawn along it.
	const float Along = length(Input.Relative);
	const float3 Direction = Input.Relative / Along;
	const float Stored = Texture0.SampleLevel(PointClamp, Input.Clip.xy * ScreenSize.zw, 0.0).r;
	const float Reach = Stored > 0.0 ? Options.z / Stored * Along / Input.ViewDepth : 1e9;

	const float Jitter = frac(52.9829189 * frac(dot(Input.Clip.xy, float2(0.06711056, 0.00583715))));
	const float4 Flame = MarchFlame(-Input.Foot, Direction, Reach, Input.Height, Input.Breadth, Input.Seed, CameraPosition.w, Jitter);
	return float4(Flame.rgb, 0.0);
}
