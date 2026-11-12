#include "ChunkBuffers.h"
#include "Core/Log.h"
#include "Core/Png.h"
#include "GameWorld.h"
#include "Gaze.h"
#include "MagicField.h"
#include "Menu.h"
#include "Physics/Physics.h"
#include "Platform/Window.h"
#include "Render/Renderer.h"
#include "Rhi/Rhi.h"
#include "Settings.h"
#include "StarMap.h"
#include "Storms.h"
#include "SurfaceTextures.h"
#include "TeinoraSim.h"
#include "WorldMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Teinora;

namespace
{
	constexpr int SurfaceTextureSize = 1024;
	constexpr int CloudNoiseSize = 64;
	constexpr double PhysicsStep = 1.0 / 120.0;
	constexpr int MaxPhysicsSteps = 64; // enough to keep up at fifty times the usual speed, at about 60 frames a second
	constexpr double DayLength = DaySeconds; // seconds of the world's time, see ReadClock in SceneTypes.h
	constexpr std::size_t MaxDrawnLakes = 6;

	struct Options
	{
		Rhi::EBackend Backend = Rhi::EBackend::Auto;
		bool bBackendSet = false;
		bool bValidation = false;
		int Frames = 0;             // quit after this many frames with the world loaded
		std::string Screenshot;     // saved on the last of those frames
		double Hour = 10.0;
		double Day = 1.0;           // which day, for what goes on in the sky
		std::string StarChart;      // writes a picture of the star map here and quits
		std::string WorldMap;       // likewise, of the land from above
		double MapSpan = 16000.0;   // how many metres that picture covers
		double MapX = 0.0;          // and its middle
		double MapY = 0.0;
		bool bCameraSet = false;
		Vec3 Camera;
		double GroundHeight = -1.0; // puts the camera this high over the ground, whatever the camera option says
		int Detail = -1;            // overrides the setting for this run
		double Yaw = 0.8;
		double Pitch = -0.12;
		double GazeYaw = 0.0;       // starting offset of the eye from the camera, in radians
		double GazePitch = 0.0;
		int Upscaling = -1;         // overrides the setting for this run
		int LowLatency = -1;        // likewise
		int RayTracing = -1;        // likewise
		bool bNoFocus = false;      // likewise, turning the eye focus off
		double FlyStep = 0.0;       // scripted flight in an automated run: metres along the view each frame
		double FlyTurn = 0.0;       // and radians of yaw, per frame so the path ignores the frame rate
		Menu::EPage Page = Menu::EPage::Closed;
		int Throws = 0;             // balls thrown during an automated run
		int TimeStep = -1;          // the speed of time for this run, as an index into WorldSettings::GetTimeRate, unsaved
		double ThrowHold = 2.0;     // and how many seconds of holding each one stands for
		bool bMagicView = false;    // starts in the magic view
		double Strike = 0.0;        // an automated run ends with a bolt this far ahead
		int StrikeFrame = -1;       // or has it at this frame
		double Rain = -1.0;         // a run can ask for rain this hard and as stormy, from 0 to 1, whatever the clouds say
	};

	bool StartsWith(const char* Text, const char* Prefix)
	{
		return std::strncmp(Text, Prefix, std::strlen(Prefix)) == 0;
	}

	Options ParseOptions(int Count, char** Arguments)
	{
		Options Result;
		for (int Index = 1; Index < Count; ++Index)
		{
			const char* Argument = Arguments[Index];
			if (StartsWith(Argument, "--backend="))
			{
				const char* Value = Argument + 10;
				Result.bBackendSet = true;
				Result.Backend = std::strcmp(Value, "d3d12") == 0 ? Rhi::EBackend::D3D12
					: (std::strcmp(Value, "vulkan") == 0 ? Rhi::EBackend::Vulkan : Rhi::EBackend::Auto);
			}
			else if (std::strcmp(Argument, "--validation") == 0)
			{
				Result.bValidation = true;
			}
			else if (StartsWith(Argument, "--frames="))
			{
				Result.Frames = std::atoi(Argument + 9);
			}
			else if (StartsWith(Argument, "--screenshot="))
			{
				Result.Screenshot = Argument + 13;
			}
			else if (StartsWith(Argument, "--hour="))
			{
				Result.Hour = std::atof(Argument + 7);
			}
			else if (StartsWith(Argument, "--day="))
			{
				Result.Day = std::atof(Argument + 6);
			}
			else if (StartsWith(Argument, "--star-chart="))
			{
				Result.StarChart = Argument + 13;
			}
			else if (StartsWith(Argument, "--world-map="))
			{
				Result.WorldMap = Argument + 12;
			}
			else if (StartsWith(Argument, "--map-area="))
			{
				std::sscanf(Argument + 11, "%lf,%lf,%lf", &Result.MapSpan, &Result.MapX, &Result.MapY);
			}
			else if (StartsWith(Argument, "--throws="))
			{
				Result.Throws = std::atoi(Argument + 9);
			}
			else if (StartsWith(Argument, "--time-step="))
			{
				Result.TimeStep = std::atoi(Argument + 12);
			}
			else if (StartsWith(Argument, "--throw-hold="))
			{
				Result.ThrowHold = std::atof(Argument + 13);
			}
			else if (StartsWith(Argument, "--strike-frame="))
			{
				Result.StrikeFrame = std::atoi(Argument + 15);
			}
			else if (StartsWith(Argument, "--strike="))
			{
				Result.Strike = std::atof(Argument + 9);
			}
			else if (StartsWith(Argument, "--rain="))
			{
				Result.Rain = std::clamp(std::atof(Argument + 7), 0.0, 1.0);
			}
			else if (std::strcmp(Argument, "--magic") == 0)
			{
				Result.bMagicView = true;
			}
			else if (StartsWith(Argument, "--camera="))
			{
				Result.bCameraSet = std::sscanf(Argument + 9, "%lf,%lf,%lf,%lf,%lf",
					&Result.Camera.X, &Result.Camera.Y, &Result.Camera.Z, &Result.Yaw, &Result.Pitch) >= 3;
			}
			else if (StartsWith(Argument, "--ground="))
			{
				Result.GroundHeight = std::atof(Argument + 9);
			}
			else if (StartsWith(Argument, "--detail="))
			{
				Result.Detail = std::atoi(Argument + 9);
			}
			else if (StartsWith(Argument, "--upscale="))
			{
				Result.Upscaling = std::atoi(Argument + 10);
			}
			else if (StartsWith(Argument, "--reflex="))
			{
				Result.LowLatency = std::atoi(Argument + 9);
			}
			else if (StartsWith(Argument, "--raytracing="))
			{
				Result.RayTracing = std::atoi(Argument + 13);
			}
			else if (std::strcmp(Argument, "--no-focus") == 0)
			{
				Result.bNoFocus = true;
			}
			else if (StartsWith(Argument, "--fly="))
			{
				std::sscanf(Argument + 6, "%lf,%lf", &Result.FlyStep, &Result.FlyTurn);
			}
			else if (StartsWith(Argument, "--gaze="))
			{
				std::sscanf(Argument + 7, "%lf,%lf", &Result.GazeYaw, &Result.GazePitch);
			}
			else if (StartsWith(Argument, "--menu="))
			{
				const char* Value = Argument + 7;
				Result.Page = std::strcmp(Value, "settings") == 0 ? Menu::EPage::Graphics
					: (std::strcmp(Value, "controls") == 0 ? Menu::EPage::Controls
					: (std::strcmp(Value, "world") == 0 ? Menu::EPage::World : Menu::EPage::Main));
			}
		}
		return Result;
	}

	struct FreeCamera
	{
		Vec3 Position;
		Gaze View;

		static constexpr double Speed = 10.0;
		static constexpr double FastSpeed = 50.0;

		Vec3 GetForward() const { return View.GetForward(); }
		Vec3 GetGaze() const { return View.GetLineOfSight(); }

		Vec3 GetRight() const
		{
			return { std::sin(View.Yaw), -std::cos(View.Yaw), 0.0 };
		}

		void Update(const Window& Input, const ControlSettings& Controls, bool bEyeFocus, double Dt)
		{
			const double Rate = Controls.MouseSensitivity / 100.0;
			const double TurnX = -Input.GetMouseDeltaX() * Rate;
			const double TurnY = -Input.GetMouseDeltaY() * Rate * (Controls.bInvertMouse ? -1.0 : 1.0);
			if (bEyeFocus)
			{
				View.Turn(TurnX, TurnY, Dt);
			}
			else
			{
				View.TurnCamera(TurnX, TurnY);
			}

			const auto Held = [&Input, &Controls](EAction Action) { return Input.IsKeyDown(Controls.Get(Action)); };

			Vec3 Move;
			if (Held(EAction::Forward)) { Move += GetForward(); }
			if (Held(EAction::Back)) { Move -= GetForward(); }
			if (Held(EAction::Right)) { Move += GetRight(); }
			if (Held(EAction::Left)) { Move -= GetRight(); }
			if (Held(EAction::Up)) { Move += Vec3{ 0.0, 0.0, 1.0 }; }
			if (Held(EAction::Down)) { Move -= Vec3{ 0.0, 0.0, 1.0 }; }

			Position += Normalize(Move) * ((Held(EAction::Fast) ? FastSpeed : Speed) * Dt);

			// The camera may fly anywhere and dive under water, but not go through the ground.
			Position.Z = std::max(Position.Z, Sim::TerrainHeight(Position.X, Position.Y, Sim::WorldSeed) + 1.2);
		}
	};


	/** How the screen shows what cannot be seen in the world. */
	struct HudExtras
	{
		bool bMagic = false;       // whether there is magic to report at all
		Sim::MagicMix Magic;       // at the camera
		StormField::Reading Cloud; // over the camera
		bool bMagicView = false;
		double ThrowCharge = -1.0; // share of the strongest throw while the button is held, otherwise below zero
		double TimeRate = 1.0;     // how many times faster than usual the world goes by
	};

	/** A speed of time as the HUD and the menu show it: x1/50 up to x50. */
	std::string FormatTimeRate(double Rate)
	{
		char Text[32];
		std::snprintf(Text, sizeof(Text), Rate < 1.0 ? "x1/%g" : "x%g", Rate < 1.0 ? 1.0 / Rate : Rate);
		return Text;
	}

	void DrawHud(Renderer& Output, const Window& Input, const Rhi::Device& Device, const FreeCamera& Camera, const Physics::World& Physics,
		const GameWorld& World, const ControlSettings& Controls, double Day, double Hour, const Vec3& Wind, double FramesPerSecond, const HudExtras& Extras)
	{
		constexpr Float4 Bright{ 1.0f, 1.0f, 1.0f, 1.0f };
		constexpr Float4 Soft{ 0.82f, 0.88f, 0.95f, 1.0f };
		constexpr Float4 Backdrop{ 0.0f, 0.02f, 0.05f, 0.5f };
		constexpr float Size = 18.0f;
		constexpr float Line = 24.0f;
		constexpr float Margin = 10.0f;
		constexpr float Padding = 8.0f;

		constexpr Float4 Magic{ 0.70f, 1.0f, 0.42f, 1.0f };

		char Lines[5][256];
		std::snprintf(Lines[0], sizeof(Lines[0]), "%s | %s | %.0f FPS | %.1f M triangles", Rhi::GetBackendName(Device.GetBackend()), Device.GetAdapterName().c_str(),
			FramesPerSecond, static_cast<double>(Output.GetTriangleCount()) / 1e6);
		if (const char* Upscaler = Output.GetUpscalerName())
		{
			const std::size_t Used = std::strlen(Lines[0]);
			std::snprintf(Lines[0] + Used, sizeof(Lines[0]) - Used, " | %s from %ux%u", Upscaler, Output.GetRenderWidth(), Output.GetRenderHeight());
		}
		if (Output.IsTracingRays())
		{
			const std::size_t Used = std::strlen(Lines[0]);
			std::snprintf(Lines[0] + Used, sizeof(Lines[0]) - Used, " | Ray traced");
		}

		const ClockReading Clock = ReadClock(Hour); // the world's hours, minutes and seconds
		std::snprintf(Lines[1], sizeof(Lines[1]), "Day %.0f   Time %02d:%02d:%02d %s   Wind %.1f m/s", Day, Clock.Hours, Clock.Minutes, Clock.Seconds, FormatTimeRate(Extras.TimeRate).c_str(), Length(Wind));

		std::snprintf(Lines[2], sizeof(Lines[2]), "Position %.0f, %.0f   Altitude %.0f m   Bodies %zu, awake %zu, contacts %zu",
			Camera.Position.X, Camera.Position.Y, Camera.Position.Z, World.GetPropCount(), Physics.GetAwakeCount(), Physics.GetContactCount());

		std::snprintf(Lines[3], sizeof(Lines[3]), "Magic density %.3f kg/m3: earth %.3f, water %.3f, air %.3f, nature %.3f, fire %.3f, neutral %.3f%s", Extras.Magic.GetTotal(),
			Extras.Magic.Get(Sim::EMagicType::Earth), Extras.Magic.Get(Sim::EMagicType::Water), Extras.Magic.Get(Sim::EMagicType::Air), Extras.Magic.Get(Sim::EMagicType::Nature),
			Extras.Magic.GetAllFire(), Extras.Magic.Get(Sim::EMagicType::Neutral), Extras.bMagicView ? "   Magic view" : "");

		std::snprintf(Lines[4], sizeof(Lines[4]), "Cloud above %.0f m deep: water %.3f, air %.3f, passing at %.0f m/s, rubbing %.0f Pa, lightning %.3f of %.2f", Extras.Cloud.CloudDepth,
			Extras.Cloud.Mix.Get(Sim::EMagicType::Water), Extras.Cloud.Mix.Get(Sim::EMagicType::Air), Extras.Cloud.Speed, Extras.Cloud.Rubbing, Extras.Cloud.Mix.Get(Sim::EMagicType::Lightning), Sim::MagicBreakdown);

		const int LineCount = Extras.bMagic ? 5 : 3;

		float Width = 0.0f;
		for (int Index = 0; Index < LineCount; ++Index)
		{
			Width = std::max(Width, Output.MeasureText(Size, Lines[Index]));
		}
		Output.DrawRect(Margin, Margin, Width + Padding * 2.0f, Line * static_cast<float>(LineCount) + Padding * 2.0f, Backdrop);

		float Y = Margin + Padding;
		for (int Index = 0; Index < LineCount; ++Index)
		{
			Output.DrawText(Margin + Padding, Y, Size, Lines[Index], Index >= 3 ? Magic : (Index == 2 ? Soft : Bright));
			Y += Line;
		}

		// The keys as they are bound now. Four single letters read as one word, like WASD.
		const auto Name = [&Controls](EAction Action) { return GetKeyName(Controls.Get(Action)); };
		const std::string Forward = Name(EAction::Forward);
		const std::string Left = Name(EAction::Left);
		const std::string Back = Name(EAction::Back);
		const std::string Right = Name(EAction::Right);
		const bool bWord = Forward.size() + Left.size() + Back.size() + Right.size() == 4;
		const std::string Gap = bWord ? "" : " ";
		const std::string Help = Forward + Gap + Left + Gap + Back + Gap + Right + " move   " + Name(EAction::Up) + " / " + Name(EAction::Down) + " up, down   "
			+ Name(EAction::Fast) + " fast   Hold LMB throw ball   + - time   " + (Extras.bMagic ? "M magic   " : "") + Name(EAction::WorldMenu) + " world   Esc menu";

		const float HelpY = static_cast<float>(Input.GetHeight()) - Margin - Line - Padding * 2.0f;
		Output.DrawRect(Margin, HelpY, Output.MeasureText(Size, Help.c_str()) + Padding * 2.0f, Line + Padding * 2.0f, Backdrop);
		Output.DrawText(Margin + Padding, HelpY + Padding, Size, Help.c_str(), Soft);

		if (Extras.ThrowCharge >= 0.0)
		{
			constexpr float BarWidth = 280.0f;
			constexpr float BarHeight = 10.0f;
			const float BarX = (static_cast<float>(Input.GetWidth()) - BarWidth) * 0.5f;
			const float BarY = static_cast<float>(Input.GetHeight()) - 70.0f;
			Output.DrawRect(BarX - 2.0f, BarY - 2.0f, BarWidth + 4.0f, BarHeight + 4.0f, Backdrop);
			Output.DrawRect(BarX, BarY, BarWidth * static_cast<float>(Extras.ThrowCharge), BarHeight, Float4{ 1.0f, 0.82f, 0.35f, 1.0f });
		}
	}
}

int main(int ArgumentCount, char** Arguments)
{
	InstallCrashHandler();

	const Options Launch = ParseOptions(ArgumentCount, Arguments);

	if (!Launch.StarChart.empty())
	{
		constexpr int ChartHeight = 1024;
		const bool bWritten = WritePng(Launch.StarChart, DrawStarChart(MakeStarMap(Sim::WorldSeed), ChartHeight), ChartHeight * 2, ChartHeight);
		Log("Star chart %s: %s", Launch.StarChart.c_str(), bWritten ? "written" : "failed");
		return bWritten ? 0 : 1;
	}

	if (!Launch.WorldMap.empty())
	{
		constexpr int MapSize = 1600;
		const bool bWritten = WritePng(Launch.WorldMap, DrawWorldMap(MapSize, Launch.MapSpan, Launch.MapX, Launch.MapY, Sim::WorldSeed), MapSize, MapSize);
		const Vec3 Spawn = Sim::FindLandSpawn(Sim::WorldSeed);
		Log("World map %s: %s. Spawn %.0f, %.0f at %.1f m", Launch.WorldMap.c_str(), bWritten ? "written" : "failed", Spawn.X, Spawn.Y, Spawn.Z);
		for (const Sim::Lake& Item : Sim::FindLakes(Launch.MapX, Launch.MapY, Launch.MapSpan * 0.5, Sim::WorldSeed))
		{
			Log("Lake at %.0f, %.0f: radius %.0f, stretched %.2f, surface at %.1f m, %.1f m deep", Item.X, Item.Y, Item.Radius, Item.Stretch, Item.Level, Item.Depth);
		}

		// The nearest places to stand at the edge of the sea.
		for (int Step = 0, Found = 0; Step < 20000 && Found < 3; ++Step)
		{
			const double Reach = 40.0 * std::sqrt(static_cast<double>(Step));
			const double X = Launch.MapX + std::cos(Step * 2.399963) * Reach;
			const double Y = Launch.MapY + std::sin(Step * 2.399963) * Reach;
			const double Height = Sim::TerrainHeight(X, Y, Sim::WorldSeed);
			if (Height > -0.2 && Height < 0.2 && !Sim::FindWater(X, Y, Sim::WorldSeed).bLake)
			{
				const Vec3 Normal = Sim::TerrainNormal(X, Y, Sim::WorldSeed);
				Log("Coast at %.0f, %.0f, facing %.2f", X, Y, std::atan2(-Normal.Y, -Normal.X));
				++Found;
			}
		}
		return bWritten ? 0 : 1;
	}

	const std::string SettingsPath = GetExecutableDirectory() + "settings.ini";
	GameSettings Settings;
	Settings.Load(SettingsPath);
	if (Launch.Upscaling >= 0)
	{
		Settings.Render.Upscaling = Launch.Upscaling;
	}
	if (Launch.LowLatency >= 0)
	{
		Settings.Render.LowLatency = Launch.LowLatency;
	}
	if (Launch.TimeStep >= 0)
	{
		Settings.World.TimeSpeed = std::clamp(Launch.TimeStep, 0, WorldSettings::TimeRates - 1);
	}
	if (Launch.RayTracing >= 0)
	{
		Settings.Render.RayTracing = Launch.RayTracing;
	}
	if (Launch.bNoFocus)
	{
		Settings.Render.bEyeFocus = false;
	}
	if (Launch.Detail >= 0)
	{
		Settings.Render.GeometryDetail = Launch.Detail;
	}
	Settings.World.Apply();

	const bool bAutomated = Launch.Frames > 0; // an automated run leaves the mouse and focus alone
	Window AppWindow(L"Winds of Teinora", 1600, 900, !bAutomated);
	if (Settings.bBorderless)
	{
		AppWindow.SetBorderless(true);
	}

	Rhi::DeviceDesc DeviceDesc;
	DeviceDesc.Backend = Launch.bBackendSet ? Launch.Backend : Settings.Backend;
	DeviceDesc.NativeWindow = AppWindow.GetNativeHandle();
	DeviceDesc.Width = static_cast<std::uint32_t>(AppWindow.GetWidth());
	DeviceDesc.Height = static_cast<std::uint32_t>(AppWindow.GetHeight());
	DeviceDesc.bValidation = Launch.bValidation;

	const std::unique_ptr<Rhi::Device> Device = Rhi::CreateDevice(DeviceDesc);
	if (Device == nullptr)
	{
		ShowErrorMessage("No supported graphics API: both Direct3D 12 and Vulkan failed to start.");
		return 1;
	}
	Log("Graphics: %s on %s", Rhi::GetBackendName(Device->GetBackend()), Device->GetAdapterName().c_str());

	Rhi::VendorFeatures* const Vendor = Device->GetVendorFeatures();
	if (Vendor != nullptr)
	{
		Vendor->SetLowLatency(static_cast<Rhi::ELowLatencyMode>(Settings.Render.LowLatency));
	}

	const MenuContext Context{ Device->GetBackend(), Vendor != nullptr && Vendor->HasUpscaling(), Vendor != nullptr && Vendor->HasLowLatency(), Device->SupportsRayTracing(), !bAutomated };
	Renderer Output(*Device, DeviceDesc.Width, DeviceDesc.Height, Settings.Render);
	const SurfaceTextures Surfaces = MakeSurfaceTextures(SurfaceTextureSize);
	Output.SetSurfaceTextures(static_cast<std::uint32_t>(Surfaces.Size), static_cast<std::uint32_t>(Surfaces.Mips), static_cast<std::uint32_t>(Surfaces.Layers),
		Surfaces.Colors.data(), Surfaces.Normals.data());
	std::vector<std::uint8_t> CloudNoise = MakeCloudNoise(CloudNoiseSize);
	Output.SetCloudNoise(CloudNoise.data(), static_cast<std::uint32_t>(CloudNoiseSize));
	const std::vector<SkyStar> Stars = MakeStarMap(Sim::WorldSeed);
	Output.SetStarMap(Stars.data(), static_cast<std::uint32_t>(Stars.size()));

	Physics::World Physics;
	ChunkBuffers Terrain(*Device);
	GameWorld World(Terrain, Physics, Settings.GetViewRadius(), Settings.Render.GeometryDetail);

	std::unique_ptr<MagicSimulation> Magic;
	std::unique_ptr<StormField> Storms;
	if constexpr (Sim::bMagicEnabled)
	{
		Magic = std::make_unique<MagicSimulation>(Sim::WorldSeed);
		Storms = std::make_unique<StormField>(Sim::WorldSeed, std::move(CloudNoise), CloudNoiseSize, Sim::CloudBottom, 1.0 / 80000.0);
	}
	MagicVolume NearMagic;
	MagicVolume FarMagic;
	std::vector<MagicPush> MagicPushes;
	std::vector<MagicDeposit> MagicDeposits;
	Sim::Weather Weather;
	double Overcast = -1.0; // how closed the sky is around the camera, from 0 to 1; below zero until first read
	std::vector<float> CloudMap;
	static_assert(StormField::Side == CloudMapSide, "the renderer takes the storms' sheet as it is");
	World.SetMagic(&NearMagic);
	bool bMagicView = Magic != nullptr && Launch.bMagicView;
	float MagicFade = bMagicView ? 1.0f : 0.0f;

	double SimTime = 0.0;
	Physics.SetForceCallback([&World, &SimTime](Physics::BodyId Id, Physics::Body& Item, double)
	{
		World.ApplyForces(Id, Item, SimTime, PhysicsStep);
	});

	FreeCamera Camera;
	Camera.Position = Launch.bCameraSet ? Launch.Camera : Sim::FindLandSpawn(Sim::WorldSeed) + Vec3{ 0.0, 0.0, 14.0 };
	if (Launch.GroundHeight >= 0.0)
	{
		Camera.Position.Z = Sim::TerrainHeight(Camera.Position.X, Camera.Position.Y, Sim::WorldSeed) + Launch.GroundHeight;
	}
	Camera.View.Aim(Launch.bCameraSet ? Launch.Yaw : Sim::FindInlandYaw(Camera.Position, Sim::WorldSeed), Launch.Pitch, Launch.GazeYaw, Launch.GazePitch);

	Menu GameMenu;
	GameMenu.SetPage(Launch.Page);

	WorldClock Clock{ Launch.Hour, Launch.Day };
	double Accumulator = 0.0;
	double FramesPerSecond = 0.0;
	Vec3 WindOffset;
	Vec3 CloudDrift; // how far the wind aloft has carried the clouds, and in Z how far they have changed
	int SettledFrames = 0;
	int ThrowsLeft = Launch.Throws;
	double WorldDt = 0.0;        // the world's seconds that this frame got through
	double CountedFromSim = 0.0; // where the world's time and the gas's stood when an automated run began to count frames
	double CountedFromGas = 0.0;
	double CountedReal = 0.0;    // and the seconds that have gone by since
	double ThrowHeld = -1.0; // seconds the button has been down for, below zero while it is up
	std::vector<ObjectDraw> Objects;
	std::vector<WaterDraw> Waters;
	std::vector<PlantMove> PlantMoves;
	std::vector<HotLight> HotLights;
	std::vector<PuffDraw> Puffs;
	std::vector<FireContact> FireContacts;
	std::vector<BoltDraw> Bolts;
	std::vector<BoltStrike> Strikes;
	bool bLiquidShown = false;
	std::uint32_t RenderedWidth = DeviceDesc.Width;
	std::uint32_t RenderedHeight = DeviceDesc.Height;

	auto LastTime = std::chrono::steady_clock::now();
	while (AppWindow.PollEvents())
	{
		const std::uint32_t Width = static_cast<std::uint32_t>(AppWindow.GetWidth());
		const std::uint32_t Height = static_cast<std::uint32_t>(AppWindow.GetHeight());
		if (Width == 0 || Height == 0)
		{
			// Minimised: nothing to draw into, and the world can wait.
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			LastTime = std::chrono::steady_clock::now();
			continue;
		}

		if (Vendor != nullptr)
		{
			Vendor->BeginSimulation();
		}

		const auto Now = std::chrono::steady_clock::now();
		const double Dt = std::min(std::chrono::duration<double>(Now - LastTime).count(), 0.1);
		LastTime = Now;
		FramesPerSecond = FramesPerSecond == 0.0 ? 1.0 / std::max(Dt, 1e-4) : FramesPerSecond * 0.95 + 0.05 / std::max(Dt, 1e-4);

		const bool bPaused = GameMenu.PausesWorld();
		const bool bHandsFree = !GameMenu.IsOpen() && !bAutomated; // a menu has the mouse and the keys while it is open
		AppWindow.SetMouseCaptured(bHandsFree);

		if (!bPaused)
		{
			if (bHandsFree)
			{
				Camera.Update(AppWindow, Settings.Controls, Settings.Render.bEyeFocus, Dt);
			}
			else if (World.IsSettled())
			{
				Camera.View.Yaw += Launch.FlyTurn;
				Camera.Position += Camera.GetForward() * Launch.FlyStep;
			}

			// The longer the button stays down, the harder the ball goes when it comes up.
			if (bHandsFree && AppWindow.WasMousePressed(EMouseButton::Left))
			{
				ThrowHeld = 0.0;
			}
			else if (ThrowHeld >= 0.0)
			{
				ThrowHeld += Dt;
			}
			if (ThrowHeld >= 0.0 && !AppWindow.IsMouseDown(EMouseButton::Left))
			{
				World.ThrowBall(Camera.Position, Camera.GetGaze(), Sim::ThrowSpeedAfterHold(ThrowHeld));
				ThrowHeld = -1.0;
			}

			if (ThrowsLeft > 0 && World.IsSettled())
			{
				--ThrowsLeft;
				World.ThrowBall(Camera.Position, Camera.GetGaze(), Sim::ThrowSpeedAfterHold(Launch.ThrowHold));
			}

			if (Magic != nullptr && AppWindow.WasKeyPressed(EKey::M))
			{
				bMagicView = !bMagicView;
			}

			// Time goes faster or slower a step at a time, and stays so.
			if (bHandsFree)
			{
				const int Step = (AppWindow.WasKeyPressed(EKey::Plus) || AppWindow.WasKeyPressed(EKey::Add) ? 1 : 0)
					- (AppWindow.WasKeyPressed(EKey::Minus) || AppWindow.WasKeyPressed(EKey::Subtract) ? 1 : 0);
				const int Wanted = std::clamp(Settings.World.TimeSpeed + Step, 0, WorldSettings::TimeRates - 1);
				if (Wanted != Settings.World.TimeSpeed)
				{
					Settings.World.TimeSpeed = Wanted;
					Settings.Save(SettingsPath);
				}
			}

			// The world runs at the speed of time that the menu sets, as far as the physics can keep up, and
			// the suns and the wind go by the time it has got through, so that nothing runs ahead of the rest.
			const double Before = SimTime;
			Accumulator = std::min(Accumulator + Dt * WorldSettings::GetTimeRate(Settings.World.TimeSpeed), PhysicsStep * MaxPhysicsSteps);
			while (Accumulator >= PhysicsStep)
			{
				World.Step(PhysicsStep);
				SimTime += PhysicsStep;
				Accumulator -= PhysicsStep;
			}
			WorldDt = SimTime - Before;

			Clock.Hour += WorldDt / DayLength * 24.0;
			if (Clock.Hour >= 24.0)
			{
				Clock.Hour -= 24.0;
				Clock.Day += 1.0;
			}
		}
		else
		{
			ThrowHeld = -1.0; // opening the menu lets go of the ball without throwing it
			WorldDt = 0.0;
		}

		World.Update(Camera.Position, SimTime);

		if (Magic != nullptr)
		{
			World.TakeMagicPushes(MagicPushes);
			Magic->AddPushes(MagicPushes);
			World.TakeMagicDeposits(MagicDeposits);
			for (const MagicDeposit& Each : MagicDeposits)
			{
				Magic->AddDeposit(Each);
			}
			Magic->SetLightDrops(GetLightDropSources(Clock.Hour, Clock.Day));
			Magic->Advance(Camera.Position, World.GetAirDrift(), SimTime, GetSunHeat(Clock.Hour, Clock.Day));
			if (Magic->TakeNear(NearMagic))
			{
				const double Span = NearMagic.CellSize;
				Output.SetMagicVolume(0, NearMagic.Pixels.data(), static_cast<std::uint32_t>(NearMagic.SizeX), static_cast<std::uint32_t>(NearMagic.SizeY), static_cast<std::uint32_t>(NearMagic.SizeZ),
					NearMagic.Corner, Vec3{ NearMagic.SizeX * Span, NearMagic.SizeY * Span, NearMagic.SizeZ * Span });
			}
			if (Magic->TakeFar(FarMagic))
			{
				const double Span = FarMagic.CellSize;
				Output.SetMagicVolume(1, FarMagic.Pixels.data(), static_cast<std::uint32_t>(FarMagic.SizeX), static_cast<std::uint32_t>(FarMagic.SizeY), static_cast<std::uint32_t>(FarMagic.SizeZ),
					FarMagic.Corner, Vec3{ FarMagic.SizeX * Span, FarMagic.SizeY * Span, FarMagic.SizeZ * Span });
			}
			MagicFade += ((bMagicView ? 1.0f : 0.0f) - MagicFade) * std::min(static_cast<float>(Dt) * 7.0f, 1.0f);

			Magic->TakeContacts(FireContacts);
			World.ReceiveFire(FireContacts, SimTime);
		}

		const Vec3 Wind = Sim::WindAt(Camera.Position, SimTime, Sim::WorldSeed);
		if (!bPaused)
		{
			WindOffset += Wind * WorldDt;

			// The wind aloft carries the clouds along faster, and the harder it blows the sooner they change.
			const Vec3 Aloft = Wind * Sim::CloudWindRatio;
			CloudDrift += Vec3{ Aloft.X, Aloft.Y, Sim::CloudChangeStill + Sim::CloudChangePerWind * Length(Aloft) } * WorldDt;
		}

		if (Storms != nullptr)
		{
			Storms->Update(Camera.Position, CloudDrift, Wind, Settings.Render.bClouds ? Settings.World.CloudCover : 0.0, GetSunHeat(Clock.Hour, Clock.Day), SimTime);
			if (Launch.Strike > 0.0 && SettledFrames == (Launch.StrikeFrame >= 0 ? Launch.StrikeFrame : Launch.Frames - 3))
			{
				Storms->ForceStrike(Camera.Position + Camera.GetGaze() * Launch.Strike);
			}
			Storms->TakeStrikes(Strikes);
			for (const BoltStrike& Strike : Strikes)
			{
				World.ReceiveStrike(Strike.Point, SimTime);
				for (const Vec3& Along : Strike.Channel)
				{
					Magic->AddDeposit({ Along, Sim::EMagicType::Lightning, Sim::MagicChannelDensity });
				}
			}
			Storms->CollectBolts(Bolts);
			Magic->SetBurning(World.GetBurning());
		}

		HudExtras Extras;
		Extras.bMagic = Magic != nullptr;
		if (Storms != nullptr)
		{
			Extras.Cloud = Storms->Read(Camera.Position);
		}

		// The weather follows the water magic in the cloud overhead: over 30 kg/m3 it rains, over 50 a storm pours
		// and darkens the sky. It comes and goes over some seconds of the world's time, and the rain brings water
		// magic down into the gas.
		{
			const Sim::Weather Wanted = Storms != nullptr && Settings.Render.bClouds
				? Sim::GetWeather(Extras.Cloud.Mix.Get(Sim::EMagicType::Water), Extras.Cloud.Mix.Get(Sim::EMagicType::Lightning))
				: Sim::Weather{};
			const double Share = std::min(WorldDt / Sim::WeatherChangeTime, 1.0);
			Weather.Rain += (Wanted.Rain - Weather.Rain) * Share;
			Weather.Storm += (Wanted.Storm - Weather.Storm) * Share;
			if (Launch.Rain >= 0.0)
			{
				Weather.Rain = Launch.Rain;
				Weather.Storm = Launch.Rain;
			}
			if (Magic != nullptr)
			{
				Magic->SetRain(Sim::RainCarries * Weather.Rain);
			}

			// How closed the sky is around the camera, which greys the light and the haze. Rain a run asks for
			// comes with the sky that would make it.
			double Gathered = Storms != nullptr && Settings.Render.bClouds ? Storms->GetGathered(Camera.Position, 1500.0) : 0.0;
			if (Launch.Rain >= 0.0)
			{
				Gathered = 0.5 + 0.5 * Launch.Rain;
			}
			const double Wanting = Sim::GetOvercast(Gathered);
			Overcast = Overcast < 0.0 || Launch.Rain >= 0.0 ? Wanting : Overcast + (Wanting - Overcast) * Share;
		}
		Extras.Magic = NearMagic.GetMix(Camera.Position);
		Extras.bMagicView = bMagicView;
		Extras.ThrowCharge = ThrowHeld >= 0.0 ? Sim::ThrowSpeedAfterHold(ThrowHeld) / Sim::MaxThrowSpeed : -1.0;
		Extras.TimeRate = WorldSettings::GetTimeRate(Settings.World.TimeSpeed);
		DrawHud(Output, AppWindow, *Device, Camera, Physics, World, Settings.Controls, Clock.Day, Clock.Hour, Wind, FramesPerSecond, Extras);

		if (GameMenu.Update(AppWindow, Output, Settings, Context, Clock))
		{
			Settings.World.Apply();
			Output.ApplySettings(Settings.Render);
			if (Vendor != nullptr)
			{
				Vendor->SetLowLatency(static_cast<Rhi::ELowLatencyMode>(Settings.Render.LowLatency));
			}
			AppWindow.SetBorderless(Settings.bBorderless);
			World.SetViewRadius(Settings.GetViewRadius());
			World.SetDetail(Settings.Render.GeometryDetail);
			Settings.Save(SettingsPath);
		}
		if (GameMenu.ShouldExit())
		{
			break;
		}

		if (Width != RenderedWidth || Height != RenderedHeight)
		{
			RenderedWidth = Width;
			RenderedHeight = Height;
			Output.Resize(Width, Height);
		}

		const bool bCounting = Launch.Frames > 0 && World.IsSettled() && (Magic == nullptr || Magic->IsReady());
		SettledFrames += bCounting ? 1 : 0;
		if (bCounting && SettledFrames == 1)
		{
			CountedFromSim = SimTime;
			CountedFromGas = Magic != nullptr ? Magic->GetTime() : 0.0;
		}
		CountedReal += bCounting && SettledFrames > 1 ? Dt : 0.0;
		const bool bLastFrame = Launch.Frames > 0 && SettledFrames >= Launch.Frames;
		if (bLastFrame && !Launch.Screenshot.empty())
		{
			Device->RequestCapture();
		}

		if (Vendor != nullptr)
		{
			Vendor->EndSimulation();
		}

		Objects.clear();
		World.CollectObjects(Objects);

		// The sea, then the nearest lakes in sight.
		const double ViewDistance = World.GetViewDistance();
		Waters.clear();
		Waters.push_back({ Sim::SeaLevel, 1.0, 0.0, {}, 1.0, 0.0 });
		std::vector<Sim::Lake> Lakes = Sim::FindLakes(Camera.Position.X, Camera.Position.Y, ViewDistance, Sim::WorldSeed);
		std::sort(Lakes.begin(), Lakes.end(), [&Camera](const Sim::Lake& A, const Sim::Lake& B)
		{
			return std::hypot(A.X - Camera.Position.X, A.Y - Camera.Position.Y) - A.GetReach() < std::hypot(B.X - Camera.Position.X, B.Y - Camera.Position.Y) - B.GetReach();
		});
		for (std::size_t Index = 0; Index < std::min<std::size_t>(Lakes.size(), MaxDrawnLakes); ++Index)
		{
			const Sim::Lake& Item = Lakes[Index];
			Waters.push_back({ Item.Level, 0.0, Item.Radius, Vec3{ Item.X, Item.Y, 0.0 }, Item.Stretch, Item.Angle });
		}

		const double WaterTop = Sim::WaterSurfaceAt(Camera.Position.X, Camera.Position.Y, SimTime, Sim::WorldSeed) + World.GetLiquid().GetHeight(Camera.Position.X, Camera.Position.Y);
		const bool bUnderwater = Camera.Position.Z < WaterTop && Sim::TerrainHeight(Camera.Position.X, Camera.Position.Y, Sim::WorldSeed) < WaterTop;

		if (World.GetLiquid().IsStirred() || !bLiquidShown)
		{
			Output.SetLiquid(World.GetLiquid().GetPixels(), LiquidField::Side);
			bLiquidShown = true;
		}
		World.GetPlants().CollectMoves(Camera.Position, PlantMoves, 8);
		World.CollectHotLights(Camera.Position, HotLights, 8);
		if (Storms != nullptr)
		{
			Storms->AddFlashes(HotLights, 8);
		}
		World.CollectPuffs(Camera.Position, Puffs, 160);

		SceneView View;
		View.CameraPosition = Camera.Position;
		View.CameraForward = Camera.GetForward();
		View.GazeDirection = Camera.GetGaze();
		View.VerticalFov = Gaze::VerticalFov;
		View.Time = SimTime;
		View.HourOfDay = Clock.Hour;
		View.DayCount = Clock.Day;
		View.WindVelocity = Wind;
		View.WindOffset = WindOffset;
		View.CloudDrift = CloudDrift;
		View.ViewDistance = ViewDistance;
		View.Chunks = &Terrain.GetDraws();
		View.Objects = &Objects;
		View.Waters = &Waters;
		View.PlantMoves = &PlantMoves;
		View.HotLights = &HotLights;
		View.Puffs = &Puffs;
		View.Bolts = &Bolts;
		View.FlashStrength = Storms != nullptr ? Storms->GetCloudFlash(View.FlashPoint) : 0.0;
		View.CameraDepth = bUnderwater ? WaterTop - Camera.Position.Z : 0.0;
		View.WaveHeight = Settings.World.WaveHeight;
		View.CloudCover = Settings.World.CloudCover + Weather.Storm * Sim::StormCloudCover;
		View.Rain = Weather.Rain;
		View.Storm = Weather.Storm;
		View.Overcast = std::max(Overcast, 0.0);
		if (Storms != nullptr && Settings.Render.bClouds)
		{
			// The clouds are drawn where the water magic up there has gathered.
			Storms->WriteCloudMap(CloudMap, View.CloudMapCornerX, View.CloudMapCornerY);
			if (Launch.Rain >= 0.0)
			{
				// Rain asked for by a run comes out of a sky as overcast as the water magic for it would make.
				std::fill(CloudMap.begin(), CloudMap.end(), static_cast<float>(0.5 + 0.5 * Launch.Rain));
			}
			View.CloudMap = &CloudMap;
			View.CloudMapCell = StormField::CellSize;
		}
		{
			// The wind across the view slants the rain: across over down, as the drops fall.
			const Vec3 Right = Normalize(Cross(Camera.GetForward(), Vec3{ 0.0, 0.0, 1.0 }));
			View.RainSlant = Dot(Wind, Right) / 9.0 * 0.6;
		}
		View.LiquidCornerX = World.GetLiquid().GetCornerX();
		View.LiquidCornerY = World.GetLiquid().GetCornerY();
		View.LiquidSpan = World.GetLiquid().GetSpan();
		View.MagicView = MagicFade;
		View.bMagicBends = NearMagic.PeakDensity > Sim::MagicDewPoint;
		Output.Render(View);

		if (bLastFrame)
		{
			std::vector<std::uint8_t> Pixels;
			std::uint32_t CaptureWidth = 0;
			std::uint32_t CaptureHeight = 0;
			if (!Launch.Screenshot.empty() && Device->TakeCapture(Pixels, CaptureWidth, CaptureHeight))
			{
				const bool bWritten = WritePng(Launch.Screenshot, Pixels, CaptureWidth, CaptureHeight);
				Log("Screenshot %s: %s", Launch.Screenshot.c_str(), bWritten ? "written" : "failed");
			}
			break;
		}
	}

	Log("Finished: %.0f FPS, %.1f M triangles, %zu bodies, %zu awake, %zu contacts", FramesPerSecond,
		static_cast<double>(Output.GetTriangleCount()) / 1e6, World.GetPropCount(), Physics.GetAwakeCount(), Physics.GetContactCount());
	Log("Camera: %.3f %.3f %.3f, yaw %.5f, pitch %.5f, eye %.3f %.3f", Camera.Position.X, Camera.Position.Y, Camera.Position.Z,
		Camera.View.Yaw, Camera.View.Pitch, Camera.View.EyeYaw, Camera.View.EyePitch);
	if (Magic != nullptr)
	{
		Log("Magic: %.3f kg/m3 at the camera, %.1f at most nearby", NearMagic.GetMix(Camera.Position).GetTotal(), NearMagic.PeakDensity);
	}

	Log("Time: %s, the world went %.1f s and its gas %.1f s while %.1f s went by", FormatTimeRate(WorldSettings::GetTimeRate(Settings.World.TimeSpeed)).c_str(),
		SimTime - CountedFromSim, Magic != nullptr ? Magic->GetTime() - CountedFromGas : 0.0, CountedReal);

	Device->WaitIdle();
	return 0;
}
