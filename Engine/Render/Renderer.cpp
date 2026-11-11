#include "Render/Renderer.h"

#include "Render/Shaders.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace Teinora
{
	namespace
	{
		using Rhi::EBlend;
		using Rhi::ECompare;
		using Rhi::ECull;
		using Rhi::EFormat;
		using Rhi::PipelineDesc;
		using Rhi::RenderPass;
		using Rhi::TextureDesc;

		constexpr int CascadeCount = 3;
		constexpr double CascadeEnds[CascadeCount] = { 40.0, 160.0, 640.0 };
		constexpr double NearPlane = 0.2;
		constexpr double ShadowDepthRange = 2400.0;
		constexpr double WaveWrap = 4096.0;
		constexpr std::uint32_t RayBodyBit = 1u << 23; // in an instance's index, a body rather than a chunk
		constexpr int WaterRings[] = { 60, 110, 170, 260 };
		constexpr int WaterSegments[] = { 64, 96, 144, 240 };
		constexpr double WaterInnerRadius = 1.5;
		constexpr double WaterOuterRadius = 12000.0;
		constexpr std::uint32_t ShadowResolutions[] = { 0, 1024, 2048, 4096 };
		constexpr int FontPixelHeight = 40;
		constexpr std::uint32_t BlurDownscale = 4;
		constexpr float FocusRadius = 0.265f;
		constexpr float FocusStrength = 0.75f;
		constexpr std::size_t MaxPlantMoves = 8; // also in Shaders/Common.hlsli
		constexpr std::size_t MaxHotLights = 8;  // and this

		struct BoltVertex
		{
			Float3 Position;
			float Across;
			float Bright;
		};

		struct PuffVertex
		{
			Float3 Position;
			float PlaceX;
			float PlaceY;
			float Thickness;
			float Seed;
			float Shade;
			float Firelight;
			float Glow;
			float Unused;
		};

		std::uint16_t ToHalf(float Value)
		{
			const std::uint32_t Bits = std::bit_cast<std::uint32_t>(Value);
			const std::uint32_t Sign = (Bits >> 16) & 0x8000u;
			const std::int32_t Exponent = static_cast<std::int32_t>((Bits >> 23) & 0xFFu) - 127 + 15;
			const std::uint32_t Fraction = (Bits >> 13) & 0x3FFu;
			if (Exponent <= 0)
			{
				return static_cast<std::uint16_t>(Sign); // too small to tell from nothing
			}
			if (Exponent >= 31)
			{
				return static_cast<std::uint16_t>(Sign | 0x7BFFu); // the largest there is
			}
			return static_cast<std::uint16_t>(Sign | (static_cast<std::uint32_t>(Exponent) << 10) | Fraction);
		}

		struct MeshVertex
		{
			float Position[3];
			std::int8_t Normal[4];
		};

		float Halton(std::uint32_t Index, std::uint32_t Base)
		{
			float Result = 0.0f;
			float Fraction = 1.0f;
			while (Index > 0)
			{
				Fraction /= static_cast<float>(Base);
				Result += Fraction * static_cast<float>(Index % Base);
				Index /= Base;
			}
			return Result;
		}

		float Lerp(float A, float B, float T)
		{
			return A + (B - A) * T;
		}

		Float3 Lerp(const Float3& A, const Float3& B, float T)
		{
			return { Lerp(A.X, B.X, T), Lerp(A.Y, B.Y, T), Lerp(A.Z, B.Z, T) };
		}

		float SmoothStep(float Edge0, float Edge1, float Value)
		{
			const float T = std::clamp((Value - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
			return T * T * (3.0f - 2.0f * T);
		}

		Float4 ToFloat4(const Float3& V, float W = 0.0f)
		{
			return { V.X, V.Y, V.Z, W };
		}

		std::int8_t PackNormal(double Value)
		{
			return static_cast<std::int8_t>(std::clamp(Value * 127.0, -127.0, 127.0));
		}

		MeshVertex MakeMeshVertex(const Vec3& Position, const Vec3& Normal)
		{
			return {
				{ static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z) },
				{ PackNormal(Normal.X), PackNormal(Normal.Y), PackNormal(Normal.Z), 0 } };
		}

		void AddQuad(std::vector<MeshVertex>& Out, const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& D, const Vec3& Normal)
		{
			for (const Vec3& Corner : { A, B, C, A, C, D })
			{
				Out.push_back(MakeMeshVertex(Corner, Normal));
			}
		}

		std::vector<MeshVertex> MakeCube()
		{
			std::vector<MeshVertex> Out;
			const Vec3 Axes[3][3] = {
				{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
				{ { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },
				{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } } };

			for (const auto& Frame : Axes)
			{
				for (const double Sign : { 1.0, -1.0 })
				{
					const Vec3 Normal = Frame[0] * Sign;
					const Vec3 U = Frame[1] * (0.5 * Sign);
					const Vec3 V = Frame[2] * 0.5;
					const Vec3 Center = Normal * 0.5;
					AddQuad(Out, Center - U - V, Center + U - V, Center + U + V, Center - U + V, Normal);
				}
			}
			return Out;
		}

		std::vector<MeshVertex> MakeSphere()
		{
			constexpr int Rings = 18;
			constexpr int Segments = 28;
			const auto Point = [](int Ring, int Segment)
			{
				const double Polar = Pi * Ring / Rings;
				const double Azimuth = 2.0 * Pi * Segment / Segments;
				return Vec3{ std::sin(Polar) * std::cos(Azimuth), std::sin(Polar) * std::sin(Azimuth), std::cos(Polar) };
			};

			std::vector<MeshVertex> Out;
			for (int Ring = 0; Ring < Rings; ++Ring)
			{
				for (int Segment = 0; Segment < Segments; ++Segment)
				{
					const Vec3 A = Point(Ring, Segment);
					const Vec3 B = Point(Ring + 1, Segment);
					const Vec3 C = Point(Ring + 1, Segment + 1);
					const Vec3 D = Point(Ring, Segment + 1);
					for (const Vec3& Corner : { A, B, C, A, C, D })
					{
						Out.push_back(MakeMeshVertex(Corner * 0.5, Corner));
					}
				}
			}
			return Out;
		}

		std::vector<MeshVertex> MakeCylinder()
		{
			constexpr int Segments = 28;
			const auto Ring = [](int Segment)
			{
				const double Azimuth = 2.0 * Pi * Segment / Segments;
				return Vec3{ std::cos(Azimuth), std::sin(Azimuth), 0.0 };
			};

			const Vec3 Up{ 0.0, 0.0, 0.5 };
			std::vector<MeshVertex> Out;
			for (int Segment = 0; Segment < Segments; ++Segment)
			{
				const Vec3 A = Ring(Segment);
				const Vec3 B = Ring(Segment + 1);
				const Vec3 NormalA = A;
				const Vec3 NormalB = B;

				for (const auto& [Corner, Normal] : { std::pair{ A * 0.5 - Up, NormalA }, std::pair{ B * 0.5 - Up, NormalB }, std::pair{ B * 0.5 + Up, NormalB },
					std::pair{ A * 0.5 - Up, NormalA }, std::pair{ B * 0.5 + Up, NormalB }, std::pair{ A * 0.5 + Up, NormalA } })
				{
					Out.push_back(MakeMeshVertex(Corner, Normal));
				}

				const Vec3 Top{ 0.0, 0.0, 1.0 };
				Out.push_back(MakeMeshVertex(Up, Top));
				Out.push_back(MakeMeshVertex(A * 0.5 + Up, Top));
				Out.push_back(MakeMeshVertex(B * 0.5 + Up, Top));
				Out.push_back(MakeMeshVertex(-Up, -Top));
				Out.push_back(MakeMeshVertex(B * 0.5 - Up, -Top));
				Out.push_back(MakeMeshVertex(A * 0.5 - Up, -Top));
			}
			return Out;
		}

		/** Rows of the clip matrix give the frustum planes (Gribb and Hartmann). */
		bool IsBoxVisible(const Float4x4& Matrix, const Float3& Min, const Float3& Max)
		{
			const float* X = Matrix.M[0];
			const float* Y = Matrix.M[1];
			const float* Z = Matrix.M[2];
			const float* W = Matrix.M[3];

			const float Planes[5][4] = {
				{ W[0] + X[0], W[1] + X[1], W[2] + X[2], W[3] + X[3] },
				{ W[0] - X[0], W[1] - X[1], W[2] - X[2], W[3] - X[3] },
				{ W[0] + Y[0], W[1] + Y[1], W[2] + Y[2], W[3] + Y[3] },
				{ W[0] - Y[0], W[1] - Y[1], W[2] - Y[2], W[3] - Y[3] },
				{ W[0] - Z[0], W[1] - Z[1], W[2] - Z[2], W[3] - Z[3] } };

			for (const auto& Plane : Planes)
			{
				const float FarthestX = Plane[0] >= 0.0f ? Max.X : Min.X;
				const float FarthestY = Plane[1] >= 0.0f ? Max.Y : Min.Y;
				const float FarthestZ = Plane[2] >= 0.0f ? Max.Z : Min.Z;
				if (Plane[0] * FarthestX + Plane[1] * FarthestY + Plane[2] * FarthestZ + Plane[3] < 0.0f)
				{
					return false;
				}
			}
			return true;
		}

		/** Inverts a general 4x4 matrix by cofactors. */
		Float4x4 Invert(const Float4x4& Matrix)
		{
			const float* M = &Matrix.M[0][0];
			float T[16];

			T[0] = M[5] * M[10] * M[15] - M[5] * M[11] * M[14] - M[9] * M[6] * M[15] + M[9] * M[7] * M[14] + M[13] * M[6] * M[11] - M[13] * M[7] * M[10];
			T[4] = -M[4] * M[10] * M[15] + M[4] * M[11] * M[14] + M[8] * M[6] * M[15] - M[8] * M[7] * M[14] - M[12] * M[6] * M[11] + M[12] * M[7] * M[10];
			T[8] = M[4] * M[9] * M[15] - M[4] * M[11] * M[13] - M[8] * M[5] * M[15] + M[8] * M[7] * M[13] + M[12] * M[5] * M[11] - M[12] * M[7] * M[9];
			T[12] = -M[4] * M[9] * M[14] + M[4] * M[10] * M[13] + M[8] * M[5] * M[14] - M[8] * M[6] * M[13] - M[12] * M[5] * M[10] + M[12] * M[6] * M[9];
			T[1] = -M[1] * M[10] * M[15] + M[1] * M[11] * M[14] + M[9] * M[2] * M[15] - M[9] * M[3] * M[14] - M[13] * M[2] * M[11] + M[13] * M[3] * M[10];
			T[5] = M[0] * M[10] * M[15] - M[0] * M[11] * M[14] - M[8] * M[2] * M[15] + M[8] * M[3] * M[14] + M[12] * M[2] * M[11] - M[12] * M[3] * M[10];
			T[9] = -M[0] * M[9] * M[15] + M[0] * M[11] * M[13] + M[8] * M[1] * M[15] - M[8] * M[3] * M[13] - M[12] * M[1] * M[11] + M[12] * M[3] * M[9];
			T[13] = M[0] * M[9] * M[14] - M[0] * M[10] * M[13] - M[8] * M[1] * M[14] + M[8] * M[2] * M[13] + M[12] * M[1] * M[10] - M[12] * M[2] * M[9];
			T[2] = M[1] * M[6] * M[15] - M[1] * M[7] * M[14] - M[5] * M[2] * M[15] + M[5] * M[3] * M[14] + M[13] * M[2] * M[7] - M[13] * M[3] * M[6];
			T[6] = -M[0] * M[6] * M[15] + M[0] * M[7] * M[14] + M[4] * M[2] * M[15] - M[4] * M[3] * M[14] - M[12] * M[2] * M[7] + M[12] * M[3] * M[6];
			T[10] = M[0] * M[5] * M[15] - M[0] * M[7] * M[13] - M[4] * M[1] * M[15] + M[4] * M[3] * M[13] + M[12] * M[1] * M[7] - M[12] * M[3] * M[5];
			T[14] = -M[0] * M[5] * M[14] + M[0] * M[6] * M[13] + M[4] * M[1] * M[14] - M[4] * M[2] * M[13] - M[12] * M[1] * M[6] + M[12] * M[2] * M[5];
			T[3] = -M[1] * M[6] * M[11] + M[1] * M[7] * M[10] + M[5] * M[2] * M[11] - M[5] * M[3] * M[10] - M[9] * M[2] * M[7] + M[9] * M[3] * M[6];
			T[7] = M[0] * M[6] * M[11] - M[0] * M[7] * M[10] - M[4] * M[2] * M[11] + M[4] * M[3] * M[10] + M[8] * M[2] * M[7] - M[8] * M[3] * M[6];
			T[11] = -M[0] * M[5] * M[11] + M[0] * M[7] * M[9] + M[4] * M[1] * M[11] - M[4] * M[3] * M[9] - M[8] * M[1] * M[7] + M[8] * M[3] * M[5];
			T[15] = M[0] * M[5] * M[10] - M[0] * M[6] * M[9] - M[4] * M[1] * M[10] + M[4] * M[2] * M[9] + M[8] * M[1] * M[6] - M[8] * M[2] * M[5];

			const float Determinant = M[0] * T[0] + M[1] * T[4] + M[2] * T[8] + M[3] * T[12];
			Float4x4 Result;
			for (int Index = 0; Index < 16; ++Index)
			{
				(&Result.M[0][0])[Index] = T[Index] / Determinant;
			}
			return Result;
		}

		struct Environment
		{
			Vec3 SunDirection;
			Vec3 Sun2Direction;
			Vec3 PlanetDirection;
			Float3 SunColor;
			Float3 Sun2Color;
			Float3 SunCorona;  // the light of each sun's corona, which the planet in front of the sun does not take away
			Float3 Sun2Corona;
			Float3 PlanetColor;
			Float3 Zenith;
			Float3 Horizon;
			float Exposure = 1.0f;
			bool bSecondSunLeads = false; // the blue-green sun is the stronger one just now
		};

		Float3 Add(const Float3& A, const Float3& B)
		{
			return { A.X + B.X, A.Y + B.Y, A.Z + B.Z };
		}

		Float3 Scale(const Float3& Color, float Amount)
		{
			return { Color.X * Amount, Color.Y * Amount, Color.Z * Amount };
		}

		float GetDaylight(float Height)
		{
			return SmoothStep(-0.08f, 0.22f, Height);
		}

		float GetDusk(float Height)
		{
			return SmoothStep(-0.14f, 0.0f, Height) * (1.0f - SmoothStep(0.0f, 0.3f, Height));
		}

		float GetRisen(float Height)
		{
			return SmoothStep(-0.02f, 0.03f, Height);
		}

		Environment ComputeEnvironment(double Hour, double DayCount, const Vec3& Eye)
		{
			// The suns' light outside the air. These two are also in Shaders/Planet.hlsli.
			constexpr Float3 SunInSpace{ 1.75f, 1.35f, 0.85f };   // yellow-orange
			constexpr Float3 Sun2InSpace{ 0.72f, 1.78f, 1.65f };  // blue-green, a little the brighter

			constexpr float PlanetAlbedo = 0.125f; // it gives back 10 to 15% of what the suns send it

			Environment Result;
			Result.SunDirection = GetSeenSun(Hour, DayCount, Eye);
			Result.Sun2Direction = GetSeenSecondSun(Hour, DayCount, Eye);
			Result.PlanetDirection = GetPlanetDirection(Hour, DayCount);

			// A sun gone behind the planet takes its part of the daylight with it, in the air too.
			const Vec3 Axis = GetPlanetAxis(DayCount + Hour / 24.0);
			const float Past = static_cast<float>(GetLightPastPlanet(Result.SunDirection, Result.PlanetDirection, Axis, SunSize));
			const float Past2 = static_cast<float>(GetLightPastPlanet(Result.Sun2Direction, Result.PlanetDirection, Axis, SecondSunSize));

			const float Height = static_cast<float>(Result.SunDirection.Z);
			const float Height2 = static_cast<float>(Result.Sun2Direction.Z);
			const float Day = GetDaylight(Height) * Past;
			const float Day2 = GetDaylight(Height2) * Past2;
			const float Dusk = GetDusk(Height) * Past;
			const float Dusk2 = GetDusk(Height2) * Past2;

			Result.SunColor = Scale(Lerp(Float3{ 1.6f, 0.66f, 0.24f }, SunInSpace, SmoothStep(0.03f, 0.45f, Height)), GetRisen(Height) * Past);
			Result.Sun2Color = Scale(Lerp(Float3{ 0.3f, 0.95f, 0.85f }, Sun2InSpace, SmoothStep(0.03f, 0.45f, Height2)), GetRisen(Height2) * Past2);
			Result.bSecondSunLeads = Height2 * Past2 > Height * Past;
			Result.SunCorona = Scale(SunInSpace, GetRisen(Height));
			Result.Sun2Corona = Scale(Sun2InSpace, GetRisen(Height2));

			// The planet sends down a share of what falls on the side of it facing the ground.
			const float Facing = 0.5f - 0.5f * static_cast<float>(Dot(Result.PlanetDirection, Result.SunDirection));
			const float Facing2 = 0.5f - 0.5f * static_cast<float>(Dot(Result.PlanetDirection, Result.Sun2Direction));
			const Float3 Reflected = Add(Scale(SunInSpace, Facing), Scale(Sun2InSpace, Facing2));
			const float PlanetUp = GetRisen(static_cast<float>(Result.PlanetDirection.Z)) * PlanetAlbedo;
			Result.PlanetColor = { Reflected.X * PlanetUp * 0.8f, Reflected.Y * PlanetUp * 0.95f, Reflected.Z * PlanetUp * 1.25f };

			// Each sun lights the air in its own colour: warm and pale, or a clear sea green.
			const float Clear = 1.0f - 0.45f * Dusk;
			const float Clear2 = 1.0f - 0.45f * Dusk2;
			Result.Zenith = Add(Add(Float3{ 0.0025f, 0.0045f, 0.012f }, Scale(Float3{ 0.10f, 0.19f, 0.46f }, Day)), Scale(Float3{ 0.02f, 0.13f, 0.20f }, Day2));
			Result.Horizon = Add(Add(Float3{ 0.008f, 0.012f, 0.024f }, Scale(Float3{ 0.46f, 0.50f, 0.52f }, Day * Clear)), Scale(Float3{ 0.12f, 0.28f, 0.32f }, Day2 * Clear2));
			Result.Horizon = Add(Add(Result.Horizon, Scale(Float3{ 0.62f, 0.24f, 0.06f }, Dusk)), Scale(Float3{ 0.06f, 0.30f, 0.28f }, Dusk2));

			// Two suns at once would burn out what one lights well.
			Result.Exposure = Lerp(1.0f, 0.62f, SmoothStep(-0.2f, 0.02f, std::max(Height, Height2))) / (1.0f + 0.3f * std::min(Day, Day2));
			return Result;
		}
	}

	struct Renderer::FrameData
	{
		Float4x4 ViewProjection;
		Float4x4 InverseViewProjection;
		Float4x4 ShadowMatrices[CascadeCount];
		Float4 CascadeEnds;
		Float4 SunDirection;
		Float4 SunColor;
		Float4 Sun2Direction;
		Float4 Sun2Color;
		Float4 PlanetDirection;
		Float4 PlanetColor;
		Float4 PlanetAxis;
		Float4 SkyZenith;
		Float4 SkyHorizon;
		Float4 CameraPosition;
		Float4 Wind;
		Float4 ScreenSize;
		Float4 Options;
		Float4 Detail;
		Float4 StarPole;
		Float4 Conditions;
		Float4 LiquidArea;
		Float4 PlantPoints[MaxPlantMoves];
		Float4 PlantMoves[MaxPlantMoves];
		Float4 HotPoints[MaxHotLights];
		Float4 HotLights[MaxHotLights];
		Float4 BoltFlash;
		Float4 HotShapes[MaxHotLights];
		Float4 CloudDrift;
		Float4 SkyShift;
		Float4 Coronas[2];
		Float4 Weather;
		Float4 Shade;
		Float4 GlowPoints[MaxMagicGlows];
		Float4 GlowColors[MaxMagicGlows];
		Float4 CloudMapPlace;
		std::uint8_t CloudMap[CloudMapSide * CloudMapSide];
	};
	Renderer::Renderer(Rhi::Device& InDevice, std::uint32_t InWindowWidth, std::uint32_t InWindowHeight, const RenderSettings& InSettings)
		: Device(InDevice), Vendor(InDevice.GetVendorFeatures()), Settings(InSettings), WindowWidth(InWindowWidth), WindowHeight(InWindowHeight)
	{
		static_assert(sizeof(FrameData) <= Rhi::MaxConstantBytes, "the frame's constants must fit what a constant buffer is bound with");
		CreatePipelines();
		CreateMeshes();
		CreateFontTexture();
		CreateSizedTargets();
		CreateShadowMap();
	}

	Renderer::~Renderer()
	{
		Device.WaitIdle();
	}

	void Renderer::CreatePipelines()
	{
		const Rhi::EBackend Backend = Device.GetBackend();

		const std::vector<Rhi::VertexStream> TerrainStreams = { { sizeof(TerrainVertex), false } };
		const std::vector<Rhi::VertexAttribute> TerrainAttributes = {
			{ EFormat::RGB32Float, 0, 0 },
			{ EFormat::RGBA8Snorm, 12, 0 },
			{ EFormat::RGBA8Unorm, 16, 0 } };

		const std::vector<Rhi::VertexStream> ObjectStreams = { { sizeof(MeshVertex), false }, { 64, true } };
		const std::vector<Rhi::VertexAttribute> ObjectAttributes = {
			{ EFormat::RGB32Float, 0, 0 },
			{ EFormat::RGBA8Snorm, 12, 0 },
			{ EFormat::RGBA32Float, 0, 1 },
			{ EFormat::RGBA32Float, 16, 1 },
			{ EFormat::RGBA32Float, 32, 1 },
			{ EFormat::RGBA32Float, 48, 1 } };

		const auto MakeOpaque = [&](const char* Vertex, const char* Pixel, const auto& Streams, const auto& Attributes)
		{
			PipelineDesc Desc;
			Desc.Vertex = GetShader(Vertex, Backend);
			Desc.Pixel = GetShader(Pixel, Backend);
			Desc.Streams = Streams;
			Desc.Attributes = Attributes;
			Desc.ColorFormat = EFormat::RGBA16Float;
			Desc.DepthFormat = EFormat::D32Float;
			Desc.bDepthTest = true;
			Desc.bDepthWrite = true;
			Desc.DepthCompare = ECompare::Greater;
			return Device.CreatePipeline(Desc);
		};

		const auto MakeShadow = [&](const char* Vertex, const auto& Streams, const auto& Attributes)
		{
			PipelineDesc Desc;
			Desc.Vertex = GetShader(Vertex, Backend);
			Desc.Streams = Streams;
			Desc.Attributes = Attributes;
			Desc.DepthFormat = EFormat::D32Float;
			Desc.bDepthTest = true;
			Desc.bDepthWrite = true;
			Desc.DepthCompare = ECompare::LessEqual;
			Desc.Cull = ECull::None; // skirts and open cones must still cast
			Desc.DepthBias = 80;
			Desc.SlopeDepthBias = 2.0f;
			return Device.CreatePipeline(Desc);
		};

		PipelineDesc LeafShadow;
		LeafShadow.Vertex = GetShader("TerrainVS", Backend);
		LeafShadow.Pixel = GetShader("FoliageShadowPS", Backend);
		LeafShadow.Streams = TerrainStreams;
		LeafShadow.Attributes = TerrainAttributes;
		LeafShadow.DepthFormat = EFormat::D32Float;
		LeafShadow.bDepthTest = true;
		LeafShadow.bDepthWrite = true;
		LeafShadow.DepthCompare = ECompare::LessEqual;
		LeafShadow.Cull = ECull::None;
		LeafShadow.DepthBias = 80;
		LeafShadow.SlopeDepthBias = 2.0f;
		FoliageShadowPipeline = Device.CreatePipeline(LeafShadow);

		const auto MakeFullscreen = [&](const char* Pixel, EFormat Format)
		{
			PipelineDesc Desc;
			Desc.Vertex = GetShader("FullscreenVS", Backend);
			Desc.Pixel = GetShader(Pixel, Backend);
			Desc.ColorFormat = Format;
			Desc.Cull = ECull::None;
			return Desc;
		};

		const auto MakeFoliage = [&](const char* Pixel)
		{
			PipelineDesc Desc;
			Desc.Vertex = GetShader("TerrainVS", Backend);
			Desc.Pixel = GetShader(Pixel, Backend);
			Desc.Streams = TerrainStreams;
			Desc.Attributes = TerrainAttributes;
			Desc.ColorFormat = EFormat::RGBA16Float;
			Desc.DepthFormat = EFormat::D32Float;
			Desc.bDepthTest = true;
			Desc.bDepthWrite = true;
			Desc.DepthCompare = ECompare::Greater;
			Desc.Cull = ECull::None; // a card is seen from both sides
			return Device.CreatePipeline(Desc);
		};

		TerrainPipeline = MakeOpaque("TerrainVS", "TerrainPS", TerrainStreams, TerrainAttributes);
		FoliagePipeline = MakeFoliage("FoliagePS");
		ObjectPipeline = MakeOpaque("ObjectVS", "ObjectPS", ObjectStreams, ObjectAttributes);
		if (Device.SupportsRayTracing())
		{
			TerrainRayPipeline = MakeOpaque("TerrainVS", "TerrainRayPS", TerrainStreams, TerrainAttributes);
			FoliageRayPipeline = MakeFoliage("FoliageRayPS");
			ObjectRayPipeline = MakeOpaque("ObjectVS", "ObjectRayPS", ObjectStreams, ObjectAttributes);
		}
		TerrainShadowPipeline = MakeShadow("TerrainVS", TerrainStreams, TerrainAttributes);
		ObjectShadowPipeline = MakeShadow("ObjectVS", ObjectStreams, ObjectAttributes);

		PipelineDesc Sky = MakeFullscreen("SkyPS", EFormat::RGBA16Float);
		Sky.DepthFormat = EFormat::D32Float;
		Sky.bDepthTest = true;
		Sky.DepthCompare = ECompare::GreaterEqual;
		SkyPipeline = Device.CreatePipeline(Sky);

		PipelineDesc Stars;
		Stars.Vertex = GetShader("StarVS", Backend);
		Stars.Pixel = GetShader("StarPS", Backend);
		Stars.Streams = { { sizeof(SkyStar), true } };
		Stars.Attributes = { { EFormat::RGBA32Float, 0, 0 }, { EFormat::RGBA8Unorm, 16, 0 } };
		Stars.ColorFormat = EFormat::RGBA16Float;
		Stars.Cull = ECull::None;
		Stars.Blend = EBlend::Additive;
		StarPipeline = Device.CreatePipeline(Stars);

		PipelineDesc Water;
		Water.Vertex = GetShader("WaterVS", Backend);
		Water.Pixel = GetShader("WaterPS", Backend);
		Water.Streams = { { sizeof(Float2), false } };
		Water.Attributes = { { EFormat::RG32Float, 0, 0 } };
		Water.ColorFormat = EFormat::RGBA16Float;
		Water.DepthFormat = EFormat::D32Float;
		Water.Cull = ECull::None; // the surface is also seen from below
		Water.Blend = EBlend::Alpha;
		Water.bDepthTest = true;
		Water.DepthCompare = ECompare::Greater;
		WaterPipeline = Device.CreatePipeline(Water);
		if (Device.SupportsRayTracing())
		{
			Water.Pixel = GetShader("WaterRayPS", Backend);
			WaterRayPipeline = Device.CreatePipeline(Water);
		}

		PipelineDesc Flames;
		Flames.Vertex = GetShader("FlameVS", Backend);
		Flames.Pixel = GetShader("FlamePS", Backend);
		Flames.ColorFormat = EFormat::RGBA16Float;
		Flames.DepthFormat = EFormat::D32Float;
		Flames.Cull = ECull::None;
		Flames.Blend = EBlend::Additive;
		Flames.bDepthTest = true;
		Flames.DepthCompare = ECompare::Greater;

		// The flames' boxes from inside, with no depth test: the shader stops each ray at what the scene has drawn.
		PipelineDesc Volumes = Flames;
		Volumes.Cull = ECull::Front;
		Volumes.bDepthTest = false;
		FlamePipeline = Device.CreatePipeline(Volumes);

		PipelineDesc Bolts = Flames;
		Bolts.Vertex = GetShader("BoltVS", Backend);
		Bolts.Pixel = GetShader("BoltPS", Backend);
		Bolts.Streams = { { sizeof(BoltVertex), false } };
		Bolts.Attributes = { { EFormat::RGB32Float, 0, 0 }, { EFormat::RG32Float, 12, 0 } };
		BoltPipeline = Device.CreatePipeline(Bolts);

		PipelineDesc Puffs = Flames;
		Puffs.Vertex = GetShader("PuffVS", Backend);
		Puffs.Pixel = GetShader("PuffPS", Backend);
		Puffs.Streams = { { sizeof(PuffVertex), false } };
		Puffs.Attributes = { { EFormat::RGB32Float, 0, 0 }, { EFormat::RG32Float, 12, 0 }, { EFormat::RGBA32Float, 20, 0 }, { EFormat::RG32Float, 36, 0 } };
		Puffs.Blend = EBlend::Alpha;
		PuffPipeline = Device.CreatePipeline(Puffs);

		PipelineDesc Underwater = MakeFullscreen("UnderwaterPS", EFormat::RGBA16Float);
		Underwater.DepthFormat = EFormat::D32Float;
		Underwater.Blend = EBlend::Alpha;
		UnderwaterPipeline = Device.CreatePipeline(Underwater);
		CopyPipeline = Device.CreatePipeline(MakeFullscreen("CopyPS", EFormat::RGBA16Float));

		MagicPipeline = Device.CreatePipeline(MakeFullscreen("MagicPS", EFormat::RGBA16Float));
		MagicBendPipeline = Device.CreatePipeline(MakeFullscreen("MagicBendPS", EFormat::RG16Float));
		TonemapPipeline = Device.CreatePipeline(MakeFullscreen("TonemapPS", EFormat::RGBA8Unorm));
		BlurPipeline = Device.CreatePipeline(MakeFullscreen("BlurPS", EFormat::RGBA8Unorm));
		MotionPipeline = Device.CreatePipeline(MakeFullscreen("MotionPS", EFormat::RG16Float));
		FinalPipeline = Device.CreatePipeline(MakeFullscreen("FinalPS", Device.GetBackbufferFormat()));

		PipelineDesc Ui;
		Ui.Vertex = GetShader("UiVS", Backend);
		Ui.Pixel = GetShader("UiPS", Backend);
		Ui.Streams = { { sizeof(UiVertex), false } };
		Ui.Attributes = { { EFormat::RG32Float, 0, 0 }, { EFormat::RG32Float, 8, 0 }, { EFormat::RGBA8Unorm, 16, 0 } };
		Ui.ColorFormat = Device.GetBackbufferFormat();
		Ui.Cull = ECull::None;
		Ui.Blend = EBlend::Alpha;
		UiPipeline = Device.CreatePipeline(Ui);
	}

	void Renderer::CreateMeshes()
	{
		const std::vector<MeshVertex> Meshes[] = { MakeCube(), MakeSphere(), MakeCylinder() };
		for (int Index = 0; Index < static_cast<int>(EPrimitive::Count); ++Index)
		{
			PrimitiveBuffers[Index] = Device.CreateBuffer(Meshes[Index].size() * sizeof(MeshVertex), Meshes[Index].data());
			PrimitiveVertices[Index] = static_cast<std::uint32_t>(Meshes[Index].size());
			if (Device.SupportsRayTracing())
			{
				PrimitiveGeometry[Index] = Device.CreateAcceleration(PrimitiveBuffers[Index], sizeof(MeshVertex), PrimitiveVertices[Index]);
			}
		}
		CreateWaterMesh();
	}

	void Renderer::CreateWaterMesh()
	{
		const int Detail = std::clamp(Settings.GeometryDetail, 0, 3);
		const int Rings = WaterRings[Detail];
		const int Segments = WaterSegments[Detail];

		// Concentric rings around the camera: fine where waves are visible, sparse towards the horizon.
		const double Growth = std::pow(WaterOuterRadius / WaterInnerRadius, 1.0 / Rings);
		std::vector<Float2> Vertices;
		Vertices.push_back({ 0.0f, 0.0f });
		double Radius = WaterInnerRadius;
		for (int Ring = 0; Ring < Rings; ++Ring)
		{
			for (int Segment = 0; Segment < Segments; ++Segment)
			{
				const double Angle = 2.0 * Pi * Segment / Segments;
				Vertices.push_back({ static_cast<float>(Radius * std::cos(Angle)), static_cast<float>(Radius * std::sin(Angle)) });
			}
			Radius *= Growth;
		}

		std::vector<std::uint32_t> Indices;
		const auto VertexAt = [Segments](int Ring, int Segment)
		{
			return static_cast<std::uint32_t>(1 + Ring * Segments + (Segment + Segments) % Segments);
		};
		for (int Segment = 0; Segment < Segments; ++Segment)
		{
			Indices.insert(Indices.end(), { 0u, VertexAt(0, Segment), VertexAt(0, Segment + 1) });
		}
		for (int Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int Segment = 0; Segment < Segments; ++Segment)
			{
				const std::uint32_t A = VertexAt(Ring, Segment);
				const std::uint32_t B = VertexAt(Ring + 1, Segment);
				const std::uint32_t C = VertexAt(Ring + 1, Segment + 1);
				const std::uint32_t D = VertexAt(Ring, Segment + 1);
				Indices.insert(Indices.end(), { A, B, C, A, C, D });
			}
		}

		WaterVertices = Device.CreateBuffer(Vertices.size() * sizeof(Float2), Vertices.data());
		WaterIndices = Device.CreateBuffer(Indices.size() * sizeof(std::uint32_t), Indices.data());
		WaterIndexCount = static_cast<std::uint32_t>(Indices.size());
	}

	void Renderer::CreateFontTexture()
	{
		Font = RasterizeFont(FontPixelHeight);
		FontSize = static_cast<float>(FontPixelHeight);

		TextureDesc Desc;
		Desc.Width = static_cast<std::uint32_t>(Font.Width);
		Desc.Height = static_cast<std::uint32_t>(Font.Height);
		Desc.Format = EFormat::RGBA8Unorm;
		FontTexture = Device.CreateTexture(Desc, Font.Pixels.data());
		Font.Pixels.clear();
	}

	void Renderer::SetSurfaceTextures(std::uint32_t Size, std::uint32_t Mips, std::uint32_t Layers, const void* Colors, const void* Normals)
	{
		TextureDesc Desc;
		Desc.Width = Size;
		Desc.Height = Size;
		Desc.MipLevels = Mips;
		Desc.Layers = Layers;
		SurfaceColors = Device.CreateTexture(Desc, Colors);
		SurfaceNormals = Device.CreateTexture(Desc, Normals);
	}

	void Renderer::SetCloudNoise(const void* Pixels, std::uint32_t Size)
	{
		TextureDesc Desc;
		Desc.Width = Size;
		Desc.Height = Size;
		Desc.Depth = Size;
		CloudNoise = Device.CreateTexture(Desc, Pixels);
	}

	void Renderer::SetLiquid(const float* Pixels, std::uint32_t Side)
	{
		const std::size_t Count = static_cast<std::size_t>(Side) * Side * 2;
		LiquidBytes.resize(Count);
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			LiquidBytes[Index] = ToHalf(Pixels[Index]);
		}

		if (LiquidTexture)
		{
			Device.UpdateTexture(LiquidTexture, LiquidBytes.data());
			return;
		}

		TextureDesc Desc;
		Desc.Width = Side;
		Desc.Height = Side;
		Desc.Format = EFormat::RG16Float;
		LiquidTexture = Device.CreateTexture(Desc, LiquidBytes.data());
	}

	void Renderer::SetMagicVolume(int Level, const void* Pixels, std::uint32_t Width, std::uint32_t Height, std::uint32_t Depth, const Vec3& Corner, const Vec3& Size)
	{
		MagicCorners[Level] = Corner;
		MagicSizes[Level] = Size;
		if (MagicVolumes[Level])
		{
			Device.UpdateTexture(MagicVolumes[Level], Pixels);
			return;
		}

		TextureDesc Desc;
		Desc.Width = Width;
		Desc.Height = Height;
		Desc.Depth = Depth;
		MagicVolumes[Level] = Device.CreateTexture(Desc, Pixels);
	}

	void Renderer::CreateSizedTargets()
	{
		const bool bUpscale = IsUpscaling();
		const std::uint32_t Percent = bUpscale ? 100 : static_cast<std::uint32_t>(Settings.RenderScale);
		PostWidth = std::max<std::uint32_t>(WindowWidth * Percent / 100, 1);
		PostHeight = std::max<std::uint32_t>(WindowHeight * Percent / 100, 1);
		TargetWidth = PostWidth;
		TargetHeight = PostHeight;
		if (bUpscale)
		{
			Vendor->GetRenderSize(static_cast<Rhi::EUpscaleMode>(Settings.Upscaling), PostWidth, PostHeight, TargetWidth, TargetHeight);
		}
		bHistoryValid = false;

		TextureDesc Desc;
		Desc.Width = TargetWidth;
		Desc.Height = TargetHeight;
		Desc.bRenderTarget = true;

		Desc.Format = EFormat::RGBA16Float;
		SceneColor = Device.CreateTexture(Desc);
		StarTarget = Device.CreateTexture(Desc);
		SceneBehindWater = Device.CreateTexture(Desc);
		Desc.Format = EFormat::D32Float;
		SceneDepth = Device.CreateTexture(Desc);
		if (bUpscale)
		{
			Desc.Format = EFormat::RG16Float;
			MotionVectors = Device.CreateTexture(Desc);
		}

		Desc.Width = PostWidth;
		Desc.Height = PostHeight;
		Desc.Format = EFormat::RGBA8Unorm;
		ToneMapped = Device.CreateTexture(Desc);

		// Magic is soft enough for half the resolution, and that keeps its long rays cheap.
		Desc.Width = std::max<std::uint32_t>(PostWidth / 2, 1);
		Desc.Height = std::max<std::uint32_t>(PostHeight / 2, 1);
		Desc.Format = EFormat::RGBA16Float;
		MagicTarget = Device.CreateTexture(Desc);
		Desc.Format = EFormat::RG16Float;
		MagicBendTarget = Device.CreateTexture(Desc);

		Desc.Format = EFormat::RGBA8Unorm;
		Desc.Width = std::max<std::uint32_t>(PostWidth / BlurDownscale, 1);
		Desc.Height = std::max<std::uint32_t>(PostHeight / BlurDownscale, 1);
		BlurredView = Device.CreateTexture(Desc);
		BlurScratch = Device.CreateTexture(Desc);

		if (bUpscale)
		{
			TextureDesc Output;
			Output.Width = PostWidth;
			Output.Height = PostHeight;
			Output.Format = EFormat::RGBA16Float;
			Output.bUnorderedAccess = true;
			Upscaled = Device.CreateTexture(Output);
		}
	}

	bool Renderer::IsUpscaling() const
	{
		return Settings.Upscaling != 0 && Vendor != nullptr && Vendor->HasUpscaling();
	}

	bool Renderer::IsTracingRays() const
	{
		return Settings.RayTracing != 0 && Device.SupportsRayTracing();
	}

	void Renderer::DestroySizedTargets()
	{
		Device.DestroyTexture(SceneColor);
		Device.DestroyTexture(StarTarget);
		Device.DestroyTexture(SceneBehindWater);
		Device.DestroyTexture(SceneDepth);
		Device.DestroyTexture(ToneMapped);
		Device.DestroyTexture(MagicTarget);
		Device.DestroyTexture(MagicBendTarget);
		Device.DestroyTexture(BlurredView);
		Device.DestroyTexture(BlurScratch);
		if (MotionVectors)
		{
			Device.DestroyTexture(MotionVectors);
			Device.DestroyTexture(Upscaled);
			MotionVectors = {};
			Upscaled = {};
		}
	}

	void Renderer::CreateShadowMap()
	{
		ShadowResolution = ShadowResolutions[std::clamp(Settings.ShadowQuality, 0, 3)];

		// With shadows off this is one texel: the shaders still declare a comparison
		// sampling of this slot, which is only valid on a depth format.
		TextureDesc Desc;
		Desc.Width = std::max<std::uint32_t>(ShadowResolution * CascadeCount, 1);
		Desc.Height = std::max<std::uint32_t>(ShadowResolution, 1);
		Desc.Format = EFormat::D32Float;
		Desc.bRenderTarget = true;
		ShadowMap = Device.CreateTexture(Desc);
	}

	void Renderer::Resize(std::uint32_t NewWidth, std::uint32_t NewHeight)
	{
		WindowWidth = NewWidth;
		WindowHeight = NewHeight;
		Device.Resize(NewWidth, NewHeight);
		DestroySizedTargets();
		CreateSizedTargets();
	}

	void Renderer::ApplySettings(const RenderSettings& NewSettings)
	{
		const bool bScaleChanged = NewSettings.RenderScale != Settings.RenderScale || NewSettings.Upscaling != Settings.Upscaling;
		const bool bShadowChanged = NewSettings.ShadowQuality != Settings.ShadowQuality;
		const bool bDetailChanged = NewSettings.GeometryDetail != Settings.GeometryDetail;
		Settings = NewSettings;

		if (bScaleChanged)
		{
			DestroySizedTargets();
			CreateSizedTargets();
		}
		if (bShadowChanged)
		{
			Device.DestroyTexture(ShadowMap);
			CreateShadowMap();
		}
		if (bDetailChanged)
		{
			Device.DestroyBuffer(WaterVertices);
			Device.DestroyBuffer(WaterIndices);
			CreateWaterMesh();
		}
	}

	void Renderer::BuildInstances(const SceneView& View)
	{
		for (std::vector<float>& List : Instances)
		{
			List.clear();
		}

		const bool bRays = IsTracingRays();
		RayInstances.clear();
		RayBuffers.assign(std::begin(PrimitiveBuffers), std::end(PrimitiveBuffers));

		if (bRays && View.Chunks != nullptr)
		{
			for (const ChunkDraw& Chunk : *View.Chunks)
			{
				if (RayBuffers.size() == Rhi::MaxRayBuffers)
				{
					break;
				}
				if (!Chunk.Geometry)
				{
					continue;
				}

				const Float3 Offset = ToFloat(Chunk.Origin - View.CameraPosition);
				Rhi::RayInstance Instance;
				Instance.Transform[0][0] = 1.0f;
				Instance.Transform[1][1] = 1.0f;
				Instance.Transform[2][2] = 1.0f;
				Instance.Transform[0][3] = Offset.X;
				Instance.Transform[1][3] = Offset.Y;
				Instance.Transform[2][3] = Offset.Z;
				Instance.Geometry = Chunk.Geometry;
				Instance.Index = static_cast<std::uint32_t>(RayBuffers.size());
				RayInstances.push_back(Instance);
				RayBuffers.push_back(Chunk.Buffer);

				if (Chunk.LeafGeometry && RayBuffers.size() < Rhi::MaxRayBuffers)
				{
					Instance.Geometry = Chunk.LeafGeometry;
					Instance.Index = static_cast<std::uint32_t>(RayBuffers.size());
					Instance.bCutOut = true;
					RayInstances.push_back(Instance);
					RayBuffers.push_back(Chunk.LeafBuffer);
				}
			}
		}

		if (View.Objects == nullptr)
		{
			return;
		}

		for (const ObjectDraw& Object : *View.Objects)
		{
			const Mat3 Rotation = ToMatrix(Object.Rotation);
			const Vec3 Relative = Object.Position - View.CameraPosition;
			std::vector<float>& List = Instances[static_cast<int>(Object.Primitive)];

			for (int Row = 0; Row < 3; ++Row)
			{
				List.push_back(static_cast<float>(Rotation.M[Row][0]) * Object.Scale.X);
				List.push_back(static_cast<float>(Rotation.M[Row][1]) * Object.Scale.Y);
				List.push_back(static_cast<float>(Rotation.M[Row][2]) * Object.Scale.Z);
				List.push_back(static_cast<float>(Row == 0 ? Relative.X : (Row == 1 ? Relative.Y : Relative.Z)));
			}

			if (bRays)
			{
				// A hit on a body has its mesh and colour to go on, packed for Water.hlsl.
				const auto Quantize = [](float Value, std::uint32_t Top)
				{
					return static_cast<std::uint32_t>(std::clamp(Value, 0.0f, 1.0f) * static_cast<float>(Top));
				};

				Rhi::RayInstance Instance;
				std::memcpy(Instance.Transform, List.data() + List.size() - 12, sizeof(Instance.Transform));
				Instance.Geometry = PrimitiveGeometry[static_cast<int>(Object.Primitive)];
				Instance.Index = RayBodyBit | (static_cast<std::uint32_t>(Object.Primitive) << 20)
					| (Quantize(Object.Color.X, 127) << 13) | (Quantize(Object.Color.Y, 127) << 6) | Quantize(Object.Color.Z, 63);
				RayInstances.push_back(Instance);
			}

			List.insert(List.end(), { Object.Color.X, Object.Color.Y, Object.Color.Z, 1.0f });
		}
	}

	void Renderer::DrawChunks(const SceneView& View, const Float4x4& Matrix, bool bShadowPass, Rhi::PipelineHandle Solid, Rhi::PipelineHandle Leaves)
	{
		if (View.Chunks == nullptr)
		{
			return;
		}

		// Solid things first, which are what hides most of the costly leaves.
		for (const bool bLeaves : { false, true })
		{
			Device.SetPipeline(bLeaves ? Leaves : Solid);
			for (const ChunkDraw& Chunk : *View.Chunks)
			{
				const Vec3 Offset = Chunk.Origin - View.CameraPosition;
				const Float3 Relative = ToFloat(Offset);
				const Float3 Min{ Relative.X, Relative.Y, static_cast<float>(Chunk.MinZ - View.CameraPosition.Z) };
				const Float3 Max{ Relative.X + Chunk.Size, Relative.Y + Chunk.Size, static_cast<float>(Chunk.MaxZ - View.CameraPosition.Z) };
				if (!IsBoxVisible(Matrix, Min, Max))
				{
					continue;
				}

				const Float4 Push = ToFloat4(Relative);
				Device.SetPushConstants(&Push, sizeof(Push));
				Device.SetVertexBuffer(0, Chunk.Buffer);
				if (bLeaves)
				{
					Device.Draw(Chunk.ShadowVertexCount - Chunk.SolidVertexCount, 1, Chunk.SolidVertexCount);
					TriangleCount += (Chunk.ShadowVertexCount - Chunk.SolidVertexCount) / 3;

					// Grass, cut out like the leaves, but too much of it to cast shadows.
					if (!bShadowPass && Chunk.VertexCount > Chunk.ShadowVertexCount)
					{
						Device.Draw(Chunk.VertexCount - Chunk.ShadowVertexCount, 1, Chunk.ShadowVertexCount);
						TriangleCount += (Chunk.VertexCount - Chunk.ShadowVertexCount) / 3;
					}
				}
				else
				{
					Device.Draw(Chunk.SolidVertexCount);
					TriangleCount += Chunk.SolidVertexCount / 3;
				}
			}
		}
	}

	void Renderer::DrawObjects()
	{
		for (int Index = 0; Index < static_cast<int>(EPrimitive::Count); ++Index)
		{
			const std::vector<float>& List = Instances[Index];
			if (List.empty())
			{
				continue;
			}
			const std::size_t Bytes = List.size() * sizeof(float);
			Device.SetVertexBuffer(0, PrimitiveBuffers[Index]);
			std::memcpy(Device.SetTransientVertexBuffer(1, Bytes), List.data(), Bytes);

			const std::uint32_t Count = static_cast<std::uint32_t>(List.size() / 16);
			Device.Draw(PrimitiveVertices[Index], Count);
			TriangleCount += static_cast<std::uint64_t>(PrimitiveVertices[Index] / 3) * Count;
		}
	}

	void Renderer::RenderShadows(const SceneView& View, const FrameData& Frame, const Float4x4* Cascades)
	{
		RenderPass Pass;
		Pass.Depth = ShadowMap;
		Pass.bClearDepth = true;
		Pass.ClearDepth = 1.0f;
		Device.BeginPass(Pass);
		Device.SetConstants(0, &Frame, sizeof(Frame));

		const Rhi::TextureHandle Textures[] = { {}, {}, SurfaceColors };
		Device.SetTextures(Textures, 3); // leaf cards cast shadows only where they have leaves

		const float Size = static_cast<float>(ShadowResolution);
		for (int Cascade = 0; Cascade < CascadeCount && ShadowResolution > 0; ++Cascade)
		{
			Device.SetViewport(Size * Cascade, 0.0f, Size, Size);
			Device.SetConstants(1, &Cascades[Cascade], sizeof(Float4x4));

			DrawChunks(View, Cascades[Cascade], true, TerrainShadowPipeline, FoliageShadowPipeline);
			Device.SetPipeline(ObjectShadowPipeline);
			DrawObjects();
		}
		Device.EndPass();
	}

	void Renderer::SetStarMap(const SkyStar* Stars, std::uint32_t Count)
	{
		if (StarBuffer)
		{
			Device.DestroyBuffer(StarBuffer);
		}
		StarBuffer = Device.CreateBuffer(sizeof(SkyStar) * Count, Stars);
		StarCount = Count;
	}

	void Renderer::RenderStars(bool bDark)
	{
		RenderPass Pass;
		Pass.Color = StarTarget;
		Pass.bClearColor = true;
		Pass.ClearColor[3] = 0.0f;
		Device.BeginPass(Pass);
		if (bDark && StarBuffer)
		{
			Device.SetPipeline(StarPipeline);
			Device.SetVertexBuffer(0, StarBuffer);
			Device.Draw(6, StarCount);
		}
		Device.EndPass();
	}

	void Renderer::RenderScene(const SceneView& View, const Float4x4& ViewProjection)
	{
		RenderPass Pass;
		Pass.Color = SceneColor;
		Pass.Depth = SceneDepth;
		Pass.Reads[0] = ShadowMap;
		Pass.Reads[1] = StarTarget;
		Pass.bClearColor = true;
		Pass.bClearDepth = true;
		Pass.ClearDepth = 0.0f;
		Device.BeginPass(Pass);

		Device.SetConstants(1, &ViewProjection, sizeof(ViewProjection));
		const Rhi::TextureHandle Textures[] = { ShadowMap, CloudNoise, SurfaceColors, SurfaceNormals };
		Device.SetTextures(Textures, 4);

		const bool bRays = IsTracingRays();
		DrawChunks(View, ViewProjection, false, bRays ? TerrainRayPipeline : TerrainPipeline, bRays ? FoliageRayPipeline : FoliagePipeline);
		Device.SetPipeline(bRays ? ObjectRayPipeline : ObjectPipeline);
		DrawObjects();

		const Rhi::TextureHandle SkyTextures[] = { StarTarget, CloudNoise };
		Device.SetTextures(SkyTextures, 2);
		Device.SetPipeline(SkyPipeline);
		Device.Draw(3);
		Device.EndPass();
	}

	void Renderer::CopySceneBehindWater()
	{
		RenderPass Pass;
		Pass.Color = SceneBehindWater;
		Pass.Reads[0] = SceneColor;
		Device.BeginPass(Pass);
		Device.SetPipeline(CopyPipeline);
		Device.SetTextures(&SceneColor, 1);
		Device.Draw(3);
		Device.EndPass();
	}

	void Renderer::RenderTransparent(const SceneView& View)
	{
		if (View.Waters == nullptr)
		{
			return;
		}

		CopySceneBehindWater();

		RenderPass Pass;
		Pass.Color = SceneColor;
		Pass.Depth = SceneDepth;
		Pass.Reads[0] = SceneDepth;
		Pass.Reads[1] = SceneBehindWater;
		Pass.bDepthReadOnly = true;
		Device.BeginPass(Pass);

		const Rhi::TextureHandle Textures[] = { SceneDepth, CloudNoise, SurfaceColors, SurfaceNormals, LiquidTexture, SceneBehindWater };
		Device.SetTextures(Textures, 6);

		Device.SetPipeline(IsTracingRays() && Settings.RayTracing >= 2 ? WaterRayPipeline : WaterPipeline);
		Device.SetVertexBuffer(0, WaterVertices);
		Device.SetIndexBuffer(WaterIndices);
		for (const WaterDraw& Body : *View.Waters)
		{
			const Float3 Middle = ToFloat(Body.Center - View.CameraPosition);
			const Float4 Push[2] = {
				{ static_cast<float>(Body.Level), static_cast<float>(Body.WaveShare), static_cast<float>(Body.Radius), 0.0f },
				{ Middle.X, Middle.Y, static_cast<float>(Body.Stretch), static_cast<float>(Body.Angle) } };
			Device.SetPushConstants(Push, sizeof(Push));
			Device.DrawIndexed(WaterIndexCount);
			TriangleCount += WaterIndexCount / 3;
		}

		// One sheet for each of them, read from the frame's data, which also skips those with no flames.
		if (View.HotLights != nullptr && !View.HotLights->empty())
		{
			Device.SetPipeline(FlamePipeline);
			Device.Draw(36, static_cast<std::uint32_t>(std::min(View.HotLights->size(), MaxHotLights)));
		}
		DrawPuffs(View);
		DrawBolts(View);

		if (View.CameraDepth > 0.0)
		{
			Device.SetPipeline(UnderwaterPipeline);
			Device.Draw(3);
		}
		Device.EndPass();
	}

	void Renderer::DrawPuffs(const SceneView& View)
	{
		if (View.Puffs == nullptr || View.Puffs->empty())
		{
			return;
		}

		// Sheets turned to face the eye, farthest first, so that the nearer ones cover them.
		std::vector<PuffVertex> Vertices;
		for (auto Puff = View.Puffs->rbegin(); Puff != View.Puffs->rend(); ++Puff)
		{
			PuffCorner Corners[5];
			const int Count = CutPuffSheet(*Puff, View.CameraPosition, Corners);
			const auto Add = [&](const PuffCorner& Corner)
			{
				Vertices.push_back({ ToFloat(Corner.Point - View.CameraPosition), static_cast<float>(Corner.Across), static_cast<float>(Corner.Up),
					static_cast<float>(Puff->Thickness), static_cast<float>(Puff->Seed), static_cast<float>(Puff->Shade), static_cast<float>(Puff->Firelight),
					static_cast<float>(Puff->Glow), 0.0f });
			};
			for (int Index = 2; Index < Count; ++Index)
			{
				Add(Corners[0]);
				Add(Corners[Index - 1]);
				Add(Corners[Index]);
			}
		}
		if (Vertices.empty())
		{
			return;
		}

		Device.SetPipeline(PuffPipeline);
		const std::size_t Bytes = Vertices.size() * sizeof(PuffVertex);
		std::memcpy(Device.SetTransientVertexBuffer(0, Bytes), Vertices.data(), Bytes);
		Device.Draw(static_cast<std::uint32_t>(Vertices.size()));
	}

	void Renderer::DrawBolts(const SceneView& View)
	{
		if (View.Bolts == nullptr || View.Bolts->empty())
		{
			return;
		}

		// A channel is thinner than a pixel from any way off, but too bright to look it.
		const double PixelSize = 2.0 * std::tan(View.VerticalFov * 0.5) / static_cast<double>(TargetHeight);
		std::vector<BoltVertex> Vertices;
		for (const BoltDraw& Bolt : *View.Bolts)
		{
			for (const BoltSegment& Segment : *Bolt.Segments)
			{
				if (Segment.Reached > Bolt.Leader)
				{
					continue;
				}

				const double Lit = (Segment.Current >= 1.0 ? std::max(Bolt.Stroke, Bolt.Faint) : std::max(Bolt.Branches, Bolt.Faint) * Segment.Current);
				if (Lit < 0.002)
				{
					continue;
				}

				const Vec3 Middle = (Segment.From + Segment.To) * 0.5 - View.CameraPosition;
				const Vec3 Side = Normalize(Cross(Segment.To - Segment.From, Middle));
				const double Width = std::max(0.5 * Segment.Current, Length(Middle) * PixelSize * 1.8);
				const float Bright = static_cast<float>(Lit);
				const Vec3 Overlap = Normalize(Segment.To - Segment.From) * Width; // so that bends leave no gaps
				const Vec3 From = Segment.From - Overlap - View.CameraPosition;
				const Vec3 To = Segment.To + Overlap - View.CameraPosition;

				const auto Corner = [&](const Vec3& End, double Across)
				{
					Vertices.push_back({ ToFloat(End + Side * (Across * Width)), static_cast<float>(Across), Bright });
				};
				Corner(From, -1.0);
				Corner(From, 1.0);
				Corner(To, 1.0);
				Corner(From, -1.0);
				Corner(To, 1.0);
				Corner(To, -1.0);
			}
		}

		if (!Vertices.empty())
		{
			Device.SetPipeline(BoltPipeline);
			const std::size_t Bytes = Vertices.size() * sizeof(BoltVertex);
			std::memcpy(Device.SetTransientVertexBuffer(0, Bytes), Vertices.data(), Bytes);
			Device.Draw(static_cast<std::uint32_t>(Vertices.size()));
		}
	}

	void Renderer::RenderUpscale(const FrameData& Frame, const Float4x4& Steady, const Float4x4& SteadyProjection, const SceneView& View, const float* Jitter, const Vec3* Axes, double Aspect)
	{
		// Clip space now to clip space then, across the camera's movement between the frames.
		const Vec3 Moved = bHistoryValid ? View.CameraPosition - PreviousCamera : Vec3{};
		Float4x4 Translation;
		for (int Index = 0; Index < 4; ++Index)
		{
			Translation.M[Index][Index] = 1.0f;
		}
		Translation.M[0][3] = static_cast<float>(Moved.X);
		Translation.M[1][3] = static_cast<float>(Moved.Y);
		Translation.M[2][3] = static_cast<float>(Moved.Z);

		const Float4x4 ClipToPrevious = (bHistoryValid ? PreviousSteady : Steady) * Translation * Invert(Steady);

		RenderPass Motion;
		Motion.Color = MotionVectors;
		Motion.Reads[0] = SceneDepth;
		Device.BeginPass(Motion);
		Device.SetPipeline(MotionPipeline);
		Device.SetConstants(1, &ClipToPrevious, sizeof(ClipToPrevious));
		Device.SetTextures(&SceneDepth, 1);
		Device.Draw(3);
		Device.EndPass();

		Rhi::UpscaleInputs Inputs;
		Inputs.Mode = static_cast<Rhi::EUpscaleMode>(Settings.Upscaling);
		Inputs.Color = SceneColor;
		Inputs.Depth = SceneDepth;
		Inputs.Motion = MotionVectors;
		Inputs.Output = Upscaled;
		Inputs.RenderWidth = TargetWidth;
		Inputs.RenderHeight = TargetHeight;
		Inputs.OutputWidth = PostWidth;
		Inputs.OutputHeight = PostHeight;
		const Float4x4 ProjectionInverse = Invert(SteadyProjection);
		const Float4x4 PreviousToClip = Invert(ClipToPrevious);
		std::memcpy(Inputs.ViewToClip, SteadyProjection.M, sizeof(Inputs.ViewToClip));
		std::memcpy(Inputs.ClipToView, ProjectionInverse.M, sizeof(Inputs.ClipToView));
		std::memcpy(Inputs.ClipToPrevClip, ClipToPrevious.M, sizeof(Inputs.ClipToPrevClip));
		std::memcpy(Inputs.PrevClipToClip, PreviousToClip.M, sizeof(Inputs.PrevClipToClip));
		Inputs.JitterX = Jitter[0];
		Inputs.JitterY = Jitter[1];
		const auto CopyAxis = [](const Vec3& Axis, float* Out)
		{
			const Float3 Value = ToFloat(Axis);
			Out[0] = Value.X;
			Out[1] = Value.Y;
			Out[2] = Value.Z;
		};
		CopyAxis(Axes[0], Inputs.Forward);
		CopyAxis(Axes[1], Inputs.Right);
		CopyAxis(Axes[2], Inputs.Up);
		Inputs.NearPlane = static_cast<float>(NearPlane);
		Inputs.VerticalFov = static_cast<float>(View.VerticalFov);
		Inputs.Aspect = static_cast<float>(Aspect);
		Inputs.bReset = !bHistoryValid;

		Vendor->Upscale(Inputs);
		Device.RestoreAfterExternal();
		Device.SetConstants(0, &Frame, sizeof(Frame));

		PreviousSteady = Steady;
		PreviousCamera = View.CameraPosition;
		bHistoryValid = true;
	}

	void Renderer::RenderMagicPass(const SceneView& View, Rhi::TextureHandle Target, Rhi::PipelineHandle Pipeline)
	{
		RenderPass Pass;
		Pass.Color = Target;
		Pass.Reads[0] = SceneDepth;
		Device.BeginPass(Pass);

		// Corners and sizes in the frame the shader works in, relative to the camera.
		const Float4 Boxes[4] = {
			ToFloat4(ToFloat(MagicCorners[0] - View.CameraPosition)),
			ToFloat4(ToFloat(MagicSizes[0])),
			ToFloat4(ToFloat(MagicCorners[1] - View.CameraPosition)),
			ToFloat4(ToFloat(MagicSizes[1])) };
		const Rhi::TextureHandle Textures[] = { SceneDepth, {}, MagicVolumes[0], MagicVolumes[1] };

		Device.SetPipeline(Pipeline);
		Device.SetPushConstants(Boxes, sizeof(Boxes));
		Device.SetTextures(Textures, 4);
		Device.Draw(3);
		Device.EndPass();
	}

	void Renderer::RenderBlur()
	{
		const auto Blur = [this](Rhi::TextureHandle Target, Rhi::TextureHandle Source, float StepX, float StepY)
		{
			RenderPass Pass;
			Pass.Color = Target;
			Pass.Reads[0] = Source;
			Device.BeginPass(Pass);

			const float Step[4] = { StepX, StepY, 0.0f, 0.0f };
			Device.SetPipeline(BlurPipeline);
			Device.SetPushConstants(Step, sizeof(Step));
			Device.SetTextures(&Source, 1);
			Device.Draw(3);
			Device.EndPass();
		};

		// Shrinking first makes the Gaussian wide without needing many samples.
		const float SmallStepX = static_cast<float>(BlurDownscale) / static_cast<float>(PostWidth);
		const float SmallStepY = static_cast<float>(BlurDownscale) / static_cast<float>(PostHeight);
		Blur(BlurredView, ToneMapped, 1.0f / static_cast<float>(PostWidth), 1.0f / static_cast<float>(PostHeight));
		Blur(BlurScratch, BlurredView, SmallStepX, 0.0f);
		Blur(BlurredView, BlurScratch, 0.0f, SmallStepY);

		// One pass of a short kernel ends abruptly and leaves a rim around small dark
		// objects. A second, tighter pass rounds that falloff off.
		Blur(BlurScratch, BlurredView, SmallStepX * 0.6f, 0.0f);
		Blur(BlurredView, BlurScratch, 0.0f, SmallStepY * 0.6f);
	}

	void Renderer::RenderFinal(const SceneView& View, bool bMagicHaze, bool bMagicBends, const Float2& FocusCenter)
	{
		const bool bUpscale = IsUpscaling();
		const Rhi::TextureHandle Source = bUpscale ? Upscaled : SceneColor;

		RenderPass Tonemap;
		Tonemap.Color = ToneMapped;
		Tonemap.Reads[0] = Source;
		Tonemap.Reads[1] = bMagicHaze ? MagicTarget : Rhi::TextureHandle{};
		Tonemap.Reads[2] = bMagicBends ? MagicBendTarget : Rhi::TextureHandle{};
		Tonemap.Reads[3] = SceneDepth; // rain falls in front of what is further than it
		Device.BeginPass(Tonemap);

		const float MagicData[4] = {
			bMagicHaze ? View.MagicView : 0.0f,
			bMagicBends ? 1.0f : 0.0f,
			2.0f / static_cast<float>(PostWidth),
			2.0f / static_cast<float>(PostHeight) };
		const Rhi::TextureHandle TonemapTextures[] = { Source, bMagicHaze ? MagicTarget : Rhi::TextureHandle{}, bMagicBends ? MagicBendTarget : Rhi::TextureHandle{}, SceneDepth };
		Device.SetPipeline(TonemapPipeline);
		Device.SetPushConstants(MagicData, sizeof(MagicData));
		Device.SetTextures(TonemapTextures, 4);
		Device.Draw(3);
		Device.EndPass();

		if (Settings.bEyeFocus)
		{
			RenderBlur();
		}

		RenderPass Final;
		Final.Color = Device.GetBackbuffer();
		Final.Reads[0] = ToneMapped;
		Final.Reads[1] = Settings.bEyeFocus ? BlurredView : Rhi::TextureHandle{};
		Device.BeginPass(Final);

		const float FinalData[8] = {
			1.0f / static_cast<float>(PostWidth),
			1.0f / static_cast<float>(PostHeight),
			Settings.bFxaa && !bUpscale ? 1.0f : 0.0f, // the upscaler already removes aliasing
			static_cast<float>(PostWidth) / static_cast<float>(PostHeight),
			FocusCenter.X,
			FocusCenter.Y,
			FocusRadius,
			Settings.bEyeFocus ? FocusStrength : 0.0f };
		const Rhi::TextureHandle FinalTextures[] = { ToneMapped, BlurredView };
		Device.SetPipeline(FinalPipeline);
		Device.SetPushConstants(FinalData, sizeof(FinalData));
		Device.SetTextures(FinalTextures, Settings.bEyeFocus ? 2 : 1);
		Device.Draw(3);

		if (!UiVertices.empty())
		{
			const float UiData[4] = { 1.0f / static_cast<float>(WindowWidth), 1.0f / static_cast<float>(WindowHeight), 0.0f, 0.0f };
			const std::size_t Bytes = UiVertices.size() * sizeof(UiVertex);
			Device.SetPipeline(UiPipeline);
			Device.SetPushConstants(UiData, sizeof(UiData));
			Device.SetTextures(&FontTexture, 1);
			std::memcpy(Device.SetTransientVertexBuffer(0, Bytes), UiVertices.data(), Bytes);
			Device.Draw(static_cast<std::uint32_t>(UiVertices.size()));
		}
		Device.EndPass();
		UiVertices.clear();
	}

	void Renderer::Render(const SceneView& View)
	{
		TriangleCount = 0;
		Device.BeginFrame();

		const Environment Sky = ComputeEnvironment(View.HourOfDay, View.DayCount, View.CameraPosition);
		const Vec3 Forward = Normalize(View.CameraForward);
		const Vec3 Right = Normalize(Cross(Forward, Vec3{ 0.0, 0.0, 1.0 }));
		const Vec3 Up = Cross(Right, Forward);
		const double Aspect = static_cast<double>(TargetWidth) / TargetHeight;

		// The world is drawn relative to the camera, so single precision holds at any distance.
		const Float4x4 SteadyProjection = MakePerspective(View.VerticalFov, Aspect, NearPlane);
		const Float4x4 CameraView = MakeView(Forward, Right, Up, Vec3{});
		const Float4x4 Steady = SteadyProjection * CameraView;

		// An upscaler rebuilds detail from a different sub-pixel offset each frame.
		const bool bUpscale = IsUpscaling();
		float Jitter[2] = { 0.0f, 0.0f };
		Float4x4 Projection = SteadyProjection;
		if (bUpscale)
		{
			const float Ratio = static_cast<float>(PostWidth) / static_cast<float>(TargetWidth);
			const std::uint32_t Phases = std::max<std::uint32_t>(8, static_cast<std::uint32_t>(std::ceil(8.0f * Ratio * Ratio)));
			JitterIndex = JitterIndex % Phases + 1;
			Jitter[0] = Halton(JitterIndex, 2) - 0.5f;
			Jitter[1] = Halton(JitterIndex, 3) - 0.5f;
			Projection.M[0][2] = 2.0f * Jitter[0] / static_cast<float>(TargetWidth);
			Projection.M[1][2] = -2.0f * Jitter[1] / static_cast<float>(TargetHeight);
		}
		const Float4x4 ViewProjection = Projection * CameraView;

		FrameData Frame;
		Frame.ViewProjection = ViewProjection;
		Frame.InverseViewProjection = Invert(Steady);

		Float4x4 CascadeMatrices[CascadeCount];
		const Vec3 LightForward = -(Sky.bSecondSunLeads ? Sky.Sun2Direction : Sky.SunDirection);
		const Vec3 LightRight = Normalize(Cross(LightForward, Vec3{ 0.31, 0.17, 0.93 }));
		const Vec3 LightUp = Cross(LightRight, LightForward);

		const bool bRays = IsTracingRays();
		if (ShadowResolution > 0 && !bRays)
		{
			// The sphere through the eye and the far corners of the view, which contains
			// the whole frustum up to a cascade's end whatever the field of view.
			const double TanY = std::tan(View.VerticalFov * 0.5);
			const double TanX = TanY * Aspect;
			const double CornerReach = (1.0 + TanX * TanX + TanY * TanY) * 0.5;

			for (int Cascade = 0; Cascade < CascadeCount; ++Cascade)
			{
				const double Radius = CascadeEnds[Cascade] * CornerReach * 1.02;
				const Vec3 Center = Forward * (CascadeEnds[Cascade] * CornerReach);

				// Snapping to whole shadow texels in world space stops edges crawling as the camera moves.
				const double Texel = 2.0 * Radius / ShadowResolution;
				const Vec3 World = View.CameraPosition + Center;
				const double SnappedX = std::round(Dot(World, LightRight) / Texel) * Texel - Dot(View.CameraPosition, LightRight);
				const double SnappedY = std::round(Dot(World, LightUp) / Texel) * Texel - Dot(View.CameraPosition, LightUp);
				const Vec3 Snapped = LightRight * SnappedX + LightUp * SnappedY + LightForward * Dot(Center, LightForward);

				const Vec3 Eye = Snapped - LightForward * (ShadowDepthRange * 0.75);
				const Float4x4 Light = MakeOrthographic(Radius, 0.0, ShadowDepthRange) * MakeView(LightForward, LightRight, LightUp, Eye);
				CascadeMatrices[Cascade] = Light;

				// Clip space to this cascade's tile in the atlas.
				Float4x4 Tile;
				Tile.M[0][0] = 0.5f / CascadeCount;
				Tile.M[0][3] = (0.5f + static_cast<float>(Cascade)) / CascadeCount;
				Tile.M[1][1] = -0.5f;
				Tile.M[1][3] = 0.5f;
				Tile.M[2][2] = 1.0f;
				Tile.M[3][3] = 1.0f;
				Frame.ShadowMatrices[Cascade] = Tile * Light;
			}
		}

		const float ShadowTexel = ShadowResolution > 0 ? 1.0f / static_cast<float>(ShadowResolution) : 0.0f;
		Frame.CascadeEnds = { static_cast<float>(CascadeEnds[0]), static_cast<float>(CascadeEnds[1]), static_cast<float>(CascadeEnds[2]), ShadowTexel };
		Frame.SunDirection = ToFloat4(ToFloat(Sky.SunDirection));
		Frame.SunColor = ToFloat4(Sky.SunColor);
		Frame.Sun2Direction = ToFloat4(ToFloat(Sky.Sun2Direction));
		Frame.Sun2Color = ToFloat4(Sky.Sun2Color);
		Frame.Coronas[0] = ToFloat4(Sky.SunCorona);
		Frame.Coronas[1] = ToFloat4(Sky.Sun2Corona);
		Frame.Weather = { static_cast<float>(View.Rain), static_cast<float>(View.Storm), static_cast<float>(View.RainSlant), View.MagicView };
		Frame.Shade = { static_cast<float>(View.Overcast), 0.0f, 0.0f, 0.0f };
		for (std::size_t Index = 0; Index < MaxMagicGlows; ++Index)
		{
			const bool bThere = View.MagicGlows != nullptr && Index < View.MagicGlows->size();
			const MagicGlow Glow = bThere ? (*View.MagicGlows)[Index] : MagicGlow{};
			const Float3 Point = ToFloat(Glow.Point - View.CameraPosition);
			Frame.GlowPoints[Index] = { Point.X, Point.Y, Point.Z, static_cast<float>(Glow.Radius) };
			Frame.GlowColors[Index] = ToFloat4(ToFloat(Glow.Color));
		}
		Frame.CloudMapPlace = {};
		if (View.CloudMap != nullptr && View.CloudMap->size() == static_cast<std::size_t>(CloudMapSide) * CloudMapSide)
		{
			Frame.CloudMapPlace = { static_cast<float>(View.CloudMapCornerX - View.CameraPosition.X), static_cast<float>(View.CloudMapCornerY - View.CameraPosition.Y),
				static_cast<float>(View.CloudMapCell), 1.0f };
			for (std::size_t Index = 0; Index < View.CloudMap->size(); ++Index)
			{
				Frame.CloudMap[Index] = static_cast<std::uint8_t>(std::clamp((*View.CloudMap)[Index] / CloudMapTop, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
		Frame.PlanetDirection = ToFloat4(ToFloat(Sky.PlanetDirection));
		Frame.PlanetColor = ToFloat4(Sky.PlanetColor);

		const double Days = View.DayCount + View.HourOfDay / 24.0;
		const Float3 Axis = ToFloat(GetPlanetAxis(Days));
		Frame.PlanetAxis = { Axis.X, Axis.Y, Axis.Z, static_cast<float>(GetPlanetSpin(Days)) };
		const Float3 Pole = ToFloat(GetSkyPole());
		Frame.StarPole = { Pole.X, Pole.Y, Pole.Z, static_cast<float>(std::fmod(Days, 1.0) * 2.0 * Pi) };
		Frame.SkyShift = ToFloat4(ToFloat(GetSkyShift(View.CameraPosition)), 0.0f);
		Frame.SkyZenith = ToFloat4(Sky.Zenith);
		Frame.SkyHorizon = ToFloat4(Sky.Horizon);
		Frame.CameraPosition = {
			static_cast<float>(std::fmod(View.CameraPosition.X, WaveWrap)),
			static_cast<float>(std::fmod(View.CameraPosition.Y, WaveWrap)),
			static_cast<float>(View.CameraPosition.Z),
			static_cast<float>(View.Time) };
		Frame.Wind = {
			static_cast<float>(View.WindVelocity.X),
			static_cast<float>(View.WindVelocity.Y),
			static_cast<float>(std::fmod(View.WindOffset.X, WaveWrap)),
			static_cast<float>(std::fmod(View.WindOffset.Y, WaveWrap)) };
		Frame.CloudDrift = {
			static_cast<float>(std::fmod(View.CloudDrift.X, WaveWrap)),
			static_cast<float>(std::fmod(View.CloudDrift.Y, WaveWrap)),
			static_cast<float>(View.CloudDrift.Z - std::floor(View.CloudDrift.Z)),
			0.0f };
		Frame.ScreenSize = {
			static_cast<float>(TargetWidth),
			static_cast<float>(TargetHeight),
			1.0f / static_cast<float>(TargetWidth),
			1.0f / static_cast<float>(TargetHeight) };
		Frame.Options = {
			Sky.Exposure,
			Settings.bClouds ? 1.0f : 0.0f,
			static_cast<float>(NearPlane),
			static_cast<float>(2.0 / View.ViewDistance) };
		Frame.Detail = { static_cast<float>(TargetWidth) / static_cast<float>(PostWidth), Sky.bSecondSunLeads ? 1.0f : 0.0f, 0.0f, 0.0f };

		std::size_t PlantMoves = 0;
		if (View.PlantMoves != nullptr)
		{
			for (const PlantMove& Move : *View.PlantMoves)
			{
				if (PlantMoves < MaxPlantMoves)
				{
					Frame.PlantPoints[PlantMoves] = ToFloat4(ToFloat(Move.Point - View.CameraPosition), static_cast<float>(Move.Radius));
					Frame.PlantMoves[PlantMoves] = ToFloat4(ToFloat(Move.Offset));
					++PlantMoves;
				}
			}
		}
		Frame.Conditions = { static_cast<float>(View.CameraDepth), static_cast<float>(View.WaveHeight), static_cast<float>(View.CloudCover), static_cast<float>(PlantMoves) };

		if (View.HotLights != nullptr)
		{
			for (std::size_t Index = 0; Index < std::min(View.HotLights->size(), MaxHotLights); ++Index)
			{
				const HotLight& Hot = (*View.HotLights)[Index];
				Frame.HotPoints[Index] = ToFloat4(ToFloat(Hot.Point - View.CameraPosition), static_cast<float>(Hot.FlameHeight));
				Frame.HotLights[Index] = ToFloat4(ToFloat(Hot.Color), static_cast<float>(Hot.Reach));
				Frame.HotShapes[Index] = { static_cast<float>(Hot.FlameBreadth), 0.0f, 0.0f, 0.0f };
			}
		}
		Frame.BoltFlash = ToFloat4(ToFloat(View.FlashPoint - View.CameraPosition), static_cast<float>(View.FlashStrength));

		const bool bLiquid = View.LiquidSpan > 0.0 && LiquidTexture;
		Frame.LiquidArea = {
			static_cast<float>(View.LiquidCornerX - View.CameraPosition.X),
			static_cast<float>(View.LiquidCornerY - View.CameraPosition.Y),
			bLiquid ? static_cast<float>(1.0 / View.LiquidSpan) : 0.0f,
			bLiquid ? 1.0f : 0.0f };

		BuildInstances(View);
		if (bRays)
		{
			Device.SetRayScene(RayInstances.data(), static_cast<std::uint32_t>(RayInstances.size()), RayBuffers.data(), static_cast<std::uint32_t>(RayBuffers.size()));
		}
		else
		{
			RenderShadows(View, Frame, CascadeMatrices);
		}

		Device.SetConstants(0, &Frame, sizeof(Frame));
		RenderStars(Sky.Zenith.X + Sky.Zenith.Y + Sky.Zenith.Z < 0.25f); // as bright as GetNight in the shaders still shows them
		RenderScene(View, ViewProjection);
		RenderTransparent(View);

		if (bUpscale)
		{
			const Vec3 Axes[3] = { Forward, Right, Up };
			RenderUpscale(Frame, Steady, SteadyProjection, View, Jitter, Axes, Aspect);
		}

		// The gaze direction as a point on screen. Clip W is the depth along the view axis.
		const Vec3 Gaze = View.GazeDirection;
		const float* M = &Steady.M[0][0];
		const float GazeX = M[0] * static_cast<float>(Gaze.X) + M[1] * static_cast<float>(Gaze.Y) + M[2] * static_cast<float>(Gaze.Z);
		const float GazeY = M[4] * static_cast<float>(Gaze.X) + M[5] * static_cast<float>(Gaze.Y) + M[6] * static_cast<float>(Gaze.Z);
		const float GazeW = M[12] * static_cast<float>(Gaze.X) + M[13] * static_cast<float>(Gaze.Y) + M[14] * static_cast<float>(Gaze.Z);

		// Magic is only worked out when there is something of it to see.
		const bool bMagicReady = MagicVolumes[0] && MagicVolumes[1];
		const bool bMagicHaze = bMagicReady && View.MagicView > 0.001f;
		const bool bMagicBends = bMagicReady && View.bMagicBends;
		if (bMagicHaze)
		{
			RenderMagicPass(View, MagicTarget, MagicPipeline);
		}
		if (bMagicBends)
		{
			RenderMagicPass(View, MagicBendTarget, MagicBendPipeline);
		}
		RenderFinal(View, bMagicHaze, bMagicBends, { GazeX / GazeW * 0.5f + 0.5f, 0.5f - GazeY / GazeW * 0.5f });

		Device.EndFrame();
	}

	void Renderer::DrawRect(float X, float Y, float Width, float Height, const Float4& Color)
	{
		const float CellU = (static_cast<float>(FontAtlas::WhiteCell % FontAtlas::Columns) + 0.5f) / FontAtlas::Columns;
		const float CellV = (static_cast<float>(FontAtlas::WhiteCell / FontAtlas::Columns) + 0.5f) / FontAtlas::Rows;

		const std::uint8_t Bytes[4] = {
			static_cast<std::uint8_t>(Color.X * 255.0f),
			static_cast<std::uint8_t>(Color.Y * 255.0f),
			static_cast<std::uint8_t>(Color.Z * 255.0f),
			static_cast<std::uint8_t>(Color.W * 255.0f) };

		const float Corners[6][2] = {
			{ X, Y }, { X + Width, Y }, { X + Width, Y + Height },
			{ X, Y }, { X + Width, Y + Height }, { X, Y + Height } };
		for (const auto& Corner : Corners)
		{
			UiVertex Vertex;
			Vertex.Position[0] = Corner[0];
			Vertex.Position[1] = Corner[1];
			Vertex.Uv[0] = CellU;
			Vertex.Uv[1] = CellV;
			std::memcpy(Vertex.Color, Bytes, 4);
			UiVertices.push_back(Vertex);
		}
	}

	void Renderer::DrawText(float X, float Y, float Size, const char* Text, const Float4& Color)
	{
		const std::uint8_t Bytes[4] = {
			static_cast<std::uint8_t>(Color.X * 255.0f),
			static_cast<std::uint8_t>(Color.Y * 255.0f),
			static_cast<std::uint8_t>(Color.Z * 255.0f),
			static_cast<std::uint8_t>(Color.W * 255.0f) };

		const float Scale = Size / FontSize;
		const float QuadWidth = static_cast<float>(Font.CellWidth) * Scale;
		const float QuadHeight = static_cast<float>(Font.CellHeight) * Scale;
		const float CellU = 1.0f / FontAtlas::Columns;
		const float CellV = 1.0f / FontAtlas::Rows;

		float PenX = X;
		for (const char* Cursor = Text; *Cursor != '\0'; ++Cursor)
		{
			const int Glyph = static_cast<unsigned char>(*Cursor) - FontAtlas::FirstGlyph;
			if (Glyph < 0 || Glyph >= FontAtlas::GlyphCount)
			{
				continue;
			}

			const float U = static_cast<float>(Glyph % FontAtlas::Columns) * CellU;
			const float V = static_cast<float>(Glyph / FontAtlas::Columns) * CellV;
			const float Corners[6][4] = {
				{ PenX, Y, U, V },
				{ PenX + QuadWidth, Y, U + CellU, V },
				{ PenX + QuadWidth, Y + QuadHeight, U + CellU, V + CellV },
				{ PenX, Y, U, V },
				{ PenX + QuadWidth, Y + QuadHeight, U + CellU, V + CellV },
				{ PenX, Y + QuadHeight, U, V + CellV } };

			for (const auto& Corner : Corners)
			{
				UiVertex Vertex;
				Vertex.Position[0] = Corner[0];
				Vertex.Position[1] = Corner[1];
				Vertex.Uv[0] = Corner[2];
				Vertex.Uv[1] = Corner[3];
				std::memcpy(Vertex.Color, Bytes, 4);
				UiVertices.push_back(Vertex);
			}
			PenX += Font.Advance[Glyph] * Scale;
		}
	}

	float Renderer::MeasureText(float Size, const char* Text) const
	{
		float Width = 0.0f;
		for (const char* Cursor = Text; *Cursor != '\0'; ++Cursor)
		{
			const int Glyph = static_cast<unsigned char>(*Cursor) - FontAtlas::FirstGlyph;
			if (Glyph >= 0 && Glyph < FontAtlas::GlyphCount)
			{
				Width += Font.Advance[Glyph];
			}
		}
		return Width * Size / FontSize;
	}
}
