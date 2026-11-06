#pragma once

#include "Platform/Window.h"
#include "Render/Renderer.h"
#include "Rhi/Rhi.h"

#include <string>

namespace Teinora
{
	enum class EAction : int
	{
		Forward,
		Back,
		Left,
		Right,
		Up,
		Down,
		Fast,
		WorldMenu,
		Count
	};

	struct ControlSettings
	{
		static constexpr int MinSensitivity = 20;
		static constexpr int MaxSensitivity = 300;
		static constexpr int SensitivityStep = 10;

		EKey Keys[static_cast<int>(EAction::Count)] = { EKey::W, EKey::S, EKey::A, EKey::D, EKey::Space, EKey::Control, EKey::Shift, EKey::Slash };
		int MouseSensitivity = 100; // percent of the usual turning rate
		bool bInvertMouse = false;

		EKey Get(EAction Action) const { return Keys[static_cast<int>(Action)]; }

		/** Gives an action a key. An action that already had it takes the old key, so no two share one. */
		void Bind(EAction Action, EKey Key);
	};

	/** What the world menu changes, which holds from one run to the next. */
	struct WorldSettings
	{
		static constexpr double MaxWindSpeed = 20.0;
		static constexpr double WindStep = 0.5;
		static constexpr int WindDirections = 8;
		static constexpr double MaxWaveHeight = 3.0;
		static constexpr double WaveStep = 0.25;
		static constexpr double MaxCloudCover = 2.0;
		static constexpr double CloudStep = 0.25;

		double WindSpeed = 3.0;  // m/s
		int WindDirection = -1;  // eighths of a turn from east, or below zero when it wanders by itself
		int TimeSpeed = 3;       // an index into GetTimeRate
		double WaveHeight = 1.0; // share of the usual
		double CloudCover = 1.0; // likewise

		/** How many times faster than usual the day goes by at each setting. Zero stops it. */
		static double GetTimeRate(int Index);
		static constexpr int TimeRates = 11;

		/** Hands the wind and the waves to the simulation. */
		void Apply() const;
	};

	struct GameSettings
	{
		static constexpr int ViewDistanceCount = 3;
		static constexpr int PresetCount = 4; // low, medium, high, ultra

		RenderSettings Render;
		ControlSettings Controls;
		WorldSettings World;
		Rhi::EBackend Backend = Rhi::EBackend::Auto; // takes effect on the next start
		bool bBorderless = false;
		int ViewDistance = 1; // index into GetViewRadius

		/** How many chunks are kept visible around the camera. */
		int GetViewRadius() const;

		/** Sets every quality option at once. */
		void ApplyPreset(int Preset);

		/** The preset the current options match, or -1 when they are a custom mix. */
		int GetPreset() const;

		/** Missing files and unknown lines leave the defaults in place. */
		void Load(const std::string& Path);
		void Save(const std::string& Path) const;
	};
}
