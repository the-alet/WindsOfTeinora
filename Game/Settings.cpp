#include "Settings.h"

#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Teinora
{
	namespace
	{
		struct Preset
		{
			int Shadows;
			int Detail;
			int View;
			bool bEffects;
		};

		constexpr Preset Presets[GameSettings::PresetCount] = {
			{ 1, 0, 0, false },
			{ 2, 1, 1, true },
			{ 2, 2, 1, true },
			{ 3, 3, 2, true } };

		const char* BackendKey(Rhi::EBackend Backend)
		{
			switch (Backend)
			{
			case Rhi::EBackend::D3D12:  return "d3d12";
			case Rhi::EBackend::Vulkan: return "vulkan";
			default:                    return "auto";
			}
		}

		// In the order of EAction.
		constexpr const char* ActionKeys[static_cast<int>(EAction::Count)] = {
			"key_forward", "key_back", "key_left", "key_right", "key_up", "key_down", "key_fast", "key_world" };
	}

	double WorldSettings::GetTimeRate(int Index)
	{
		constexpr double Rates[TimeRates] = { 1.0 / 50.0, 1.0 / 20.0, 1.0 / 10.0, 1.0 / 5.0, 1.0 / 2.0, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0 };
		return Rates[std::clamp(Index, 0, TimeRates - 1)];
	}

	void WorldSettings::Apply() const
	{
		Sim::WorldConditions Conditions;
		Conditions.WindSpeed = WindSpeed;
		Conditions.WindHeading = WindDirection >= 0 ? 2.0 * Pi * WindDirection / WindDirections : -1.0;
		Conditions.WaveHeight = WaveHeight;
		Sim::SetConditions(Conditions);
	}

	void ControlSettings::Bind(EAction Action, EKey Key)
	{
		const EKey Old = Get(Action);
		for (EKey& Held : Keys)
		{
			Held = Held == Key ? Old : Held;
		}
		Keys[static_cast<int>(Action)] = Key;
	}

	int GameSettings::GetViewRadius() const
	{
		constexpr int Radii[ViewDistanceCount] = { 6, 10, 14 };
		return Radii[std::clamp(ViewDistance, 0, ViewDistanceCount - 1)];
	}

	void GameSettings::ApplyPreset(int Index)
	{
		const Preset& Values = Presets[Index];
		Render.ShadowQuality = Values.Shadows;
		Render.GeometryDetail = Values.Detail;
		Render.RenderScale = 100;
		Render.bFxaa = true;
		Render.bClouds = Values.bEffects;
		ViewDistance = Values.View;
	}

	int GameSettings::GetPreset() const
	{
		for (int Index = 0; Index < PresetCount; ++Index)
		{
			const Preset& Values = Presets[Index];
			if (Render.ShadowQuality == Values.Shadows && Render.GeometryDetail == Values.Detail && ViewDistance == Values.View
				&& Render.RenderScale == 100 && Render.bFxaa
				&& Render.bClouds == Values.bEffects)
			{
				return Index;
			}
		}
		return -1;
	}

	void GameSettings::Load(const std::string& Path)
	{
		std::FILE* File = std::fopen(Path.c_str(), "r");
		if (File == nullptr)
		{
			return;
		}

		char Key[64];
		char Value[64];
		while (std::fscanf(File, " %63[^= ] = %63s", Key, Value) == 2)
		{
			const int Number = std::atoi(Value);
			if (std::strcmp(Key, "backend") == 0)
			{
				Backend = std::strcmp(Value, "d3d12") == 0 ? Rhi::EBackend::D3D12
					: (std::strcmp(Value, "vulkan") == 0 ? Rhi::EBackend::Vulkan : Rhi::EBackend::Auto);
			}
			else if (std::strcmp(Key, "borderless") == 0)
			{
				bBorderless = Number != 0;
			}
			else if (std::strcmp(Key, "view_distance") == 0)
			{
				ViewDistance = std::clamp(Number, 0, ViewDistanceCount - 1);
			}
			else if (std::strcmp(Key, "shadows") == 0)
			{
				Render.ShadowQuality = std::clamp(Number, 0, 3);
			}
			else if (std::strcmp(Key, "geometry_detail") == 0)
			{
				Render.GeometryDetail = std::clamp(Number, 0, 3);
			}
			else if (std::strcmp(Key, "render_scale") == 0)
			{
				Render.RenderScale = std::clamp(Number, 50, 100);
			}
			else if (std::strcmp(Key, "upscaling") == 0)
			{
				Render.Upscaling = std::clamp(Number, 0, 5);
			}
			else if (std::strcmp(Key, "low_latency") == 0)
			{
				Render.LowLatency = std::clamp(Number, 0, 2);
			}
			else if (std::strcmp(Key, "ray_tracing") == 0)
			{
				Render.RayTracing = std::clamp(Number, 0, 2);
			}
			else if (std::strcmp(Key, "eye_focus") == 0)
			{
				Render.bEyeFocus = Number != 0;
			}
			else if (std::strcmp(Key, "fxaa") == 0)
			{
				Render.bFxaa = Number != 0;
			}
			else if (std::strcmp(Key, "clouds") == 0)
			{
				Render.bClouds = Number != 0;
			}
			else if (std::strcmp(Key, "mouse_sensitivity") == 0)
			{
				Controls.MouseSensitivity = std::clamp(Number, ControlSettings::MinSensitivity, ControlSettings::MaxSensitivity);
			}
			else if (std::strcmp(Key, "invert_mouse") == 0)
			{
				Controls.bInvertMouse = Number != 0;
			}
			else if (std::strcmp(Key, "wind_speed") == 0)
			{
				World.WindSpeed = std::clamp(std::atof(Value), 0.0, WorldSettings::MaxWindSpeed);
			}
			else if (std::strcmp(Key, "wind_direction") == 0)
			{
				World.WindDirection = std::clamp(Number, -1, WorldSettings::WindDirections - 1);
			}
			else if (std::strcmp(Key, "time_step") == 0) // not time_speed, which counted along another scale
			{
				World.TimeSpeed = std::clamp(Number, 0, WorldSettings::TimeRates - 1);
			}
			else if (std::strcmp(Key, "wave_height") == 0)
			{
				World.WaveHeight = std::clamp(std::atof(Value), 0.0, WorldSettings::MaxWaveHeight);
			}
			else if (std::strcmp(Key, "cloud_cover") == 0)
			{
				World.CloudCover = std::clamp(std::atof(Value), 0.0, WorldSettings::MaxCloudCover);
			}
			else
			{
				for (int Action = 0; Action < static_cast<int>(EAction::Count); ++Action)
				{
					if (std::strcmp(Key, ActionKeys[Action]) == 0 && Number > 0 && Number < 256 && Number != static_cast<int>(EKey::Escape))
					{
						Controls.Keys[Action] = static_cast<EKey>(Number);
					}
				}
			}
		}
		std::fclose(File);
	}

	void GameSettings::Save(const std::string& Path) const
	{
		std::FILE* File = std::fopen(Path.c_str(), "w");
		if (File == nullptr)
		{
			return;
		}

		std::fprintf(File, "backend = %s\n", BackendKey(Backend));
		std::fprintf(File, "borderless = %d\n", bBorderless ? 1 : 0);
		std::fprintf(File, "view_distance = %d\n", ViewDistance);
		std::fprintf(File, "shadows = %d\n", Render.ShadowQuality);
		std::fprintf(File, "geometry_detail = %d\n", Render.GeometryDetail);
		std::fprintf(File, "render_scale = %d\n", Render.RenderScale);
		std::fprintf(File, "upscaling = %d\n", Render.Upscaling);
		std::fprintf(File, "low_latency = %d\n", Render.LowLatency);
		std::fprintf(File, "ray_tracing = %d\n", Render.RayTracing);
		std::fprintf(File, "eye_focus = %d\n", Render.bEyeFocus ? 1 : 0);
		std::fprintf(File, "fxaa = %d\n", Render.bFxaa ? 1 : 0);
		std::fprintf(File, "clouds = %d\n", Render.bClouds ? 1 : 0);
		std::fprintf(File, "mouse_sensitivity = %d\n", Controls.MouseSensitivity);
		std::fprintf(File, "invert_mouse = %d\n", Controls.bInvertMouse ? 1 : 0);
		std::fprintf(File, "wind_speed = %.2f\n", World.WindSpeed);
		std::fprintf(File, "wind_direction = %d\n", World.WindDirection);
		std::fprintf(File, "time_step = %d\n", World.TimeSpeed);
		std::fprintf(File, "wave_height = %.2f\n", World.WaveHeight);
		std::fprintf(File, "cloud_cover = %.2f\n", World.CloudCover);
		for (int Action = 0; Action < static_cast<int>(EAction::Count); ++Action)
		{
			std::fprintf(File, "%s = %d\n", ActionKeys[Action], static_cast<int>(Controls.Keys[Action]));
		}
		std::fclose(File);
	}
}
