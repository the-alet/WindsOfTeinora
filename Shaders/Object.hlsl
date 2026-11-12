#include "Lighting.hlsli"

struct VertexInput
{
	float3 Position : TEXCOORD0;
	float4 Normal : TEXCOORD1;
	float4 Row0 : TEXCOORD2; // instance transform, rows of a 3x4 matrix relative to the camera
	float4 Row1 : TEXCOORD3;
	float4 Row2 : TEXCOORD4;
	float4 Color : TEXCOORD5; // alpha is the magic it holds, see ObjectDraw in Engine/Render/SceneTypes.h
};

struct Varyings
{
	float4 Clip : SV_Position;
	float3 Relative : TEXCOORD0;
	float3 Normal : TEXCOORD1;
	float3 Albedo : TEXCOORD2;
	float ViewDepth : TEXCOORD3;
	float3 Held : TEXCOORD4; // the colour of the magic it holds inside itself, as bright as there is of it
};

Varyings VSMain(VertexInput Input)
{
	const float4 Local = float4(Input.Position, 1.0);
	const float3 Relative = float3(dot(Input.Row0, Local), dot(Input.Row1, Local), dot(Input.Row2, Local));

	// The columns hold rotation times scale. Dividing the normal by the squared
	// scale before applying them leaves rotation over scale, the inverse transpose.
	const float3x3 Linear = float3x3(Input.Row0.xyz, Input.Row1.xyz, Input.Row2.xyz);
	const float3 ColumnX = float3(Input.Row0.x, Input.Row1.x, Input.Row2.x);
	const float3 ColumnY = float3(Input.Row0.y, Input.Row1.y, Input.Row2.y);
	const float3 ColumnZ = float3(Input.Row0.z, Input.Row1.z, Input.Row2.z);
	const float3 ScaleSquared = float3(dot(ColumnX, ColumnX), dot(ColumnY, ColumnY), dot(ColumnZ, ColumnZ));

	Varyings Output;
	Output.Clip = mul(PassViewProjection, float4(Relative, 1.0));
	Output.Relative = Relative;
	Output.Normal = mul(Linear, Input.Normal.xyz / ScaleSquared);
	Output.Albedo = Input.Color.rgb;
	Output.ViewDepth = Output.Clip.w;
	Output.Held = MagicColors[int(Input.Color.a)] * (frac(Input.Color.a) / 0.99);
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float3 Color = ShadeSurface(Input.Albedo, normalize(Input.Normal), Input.Relative, Input.ViewDepth, 0.1, 0.0);
	return float4(ShowMagic(Color, Input.Held, length(Input.Relative)), 1.0);
}
