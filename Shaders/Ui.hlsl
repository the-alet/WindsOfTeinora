#include "Common.hlsli"

struct UiConstants
{
	float2 InverseWindowSize;
	float2 Padding;
};

[[vk::push_constant]] ConstantBuffer<UiConstants> Ui : register(b2);

struct VertexInput
{
	float2 Position : TEXCOORD0; // window pixels
	float2 Uv : TEXCOORD1;
	float4 Color : TEXCOORD2;
};

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Uv : TEXCOORD0;
	float4 Color : TEXCOORD1;
};

Varyings VSMain(VertexInput Input)
{
	const float2 Ndc = Input.Position * Ui.InverseWindowSize * 2.0 - 1.0;

	Varyings Output;
	Output.Clip = float4(Ndc.x, -Ndc.y, 0.0, 1.0);
	Output.Uv = Input.Uv;
	Output.Color = Input.Color;
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float Coverage = Texture0.SampleLevel(LinearClamp, Input.Uv, 0.0).a;
	return float4(Input.Color.rgb, Input.Color.a * Coverage);
}
