// The settings of the Esc menu, kept in GameUserSettings.ini.
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

// In the order the menu shows them, as in the standalone game.
enum class ETeinoraAction : int32
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

/** What the world menu changes, as in the standalone game. */
struct FTeinoraWorldSettings
{
	static constexpr double MaxWindSpeed = 20.0;
	static constexpr double WindStep = 0.5;
	static constexpr int32 WindDirections = 8;
	static constexpr double MaxWaveHeight = 3.0;
	static constexpr double WaveStep = 0.25;
	static constexpr double MaxCloudCover = 2.0;
	static constexpr double CloudStep = 0.25;
	static constexpr int32 TimeRates = 11;

	double WindSpeed = 3.0;   // m/s
	int32 WindDirection = -1; // eighths of a turn from east, or below zero when it wanders by itself
	int32 TimeSpeed = 3;      // an index into GetTimeRate
	double WaveHeight = 1.0;  // share of the usual
	double CloudCover = 1.0;  // likewise

	/** How many times faster than usual the day goes by at each setting. Zero stops it. */
	static double GetTimeRate(int32 Index);

	/** Hands the wind and the waves to the simulation. */
	void Apply() const;
};

struct FTeinoraSettings
{
	FTeinoraSettings();

	FKey Keys[static_cast<int32>(ETeinoraAction::Count)];
	int32 MouseSensitivity = 100; // percent of the usual turning rate
	bool bInvertMouse = false;

	const FKey& GetKey(ETeinoraAction Action) const { return Keys[static_cast<int32>(Action)]; }

	/** Gives an action a key. An action that already had it takes the old key, so no two share one. */
	void BindKey(int32 Action, const FKey& Key);

	int32 Quality = 2;        // Unreal's scalability: 0 low to 3 ultra
	int32 RenderScale = 100;  // percent of the window resolution
	int32 AntiAliasing = 2;   // 0 off, 1 FXAA, 2 TSR
	int32 GeometryDetail = 1; // 0 low to 3 ultra, as in the standalone game
	int32 ViewDistance = 1;   // 0 near, 1 medium, 2 far
	bool bBorderless = false;
	bool bEyeFocus = true;
	bool bClouds = true;
	FTeinoraWorldSettings World;

	int32 GetViewRadius() const;

	void Load();
	void Save() const;
};
