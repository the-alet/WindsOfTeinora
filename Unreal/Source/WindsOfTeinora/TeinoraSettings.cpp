#include "TeinoraSettings.h"

#include "Misc/ConfigCacheIni.h"

THIRD_PARTY_INCLUDES_START
#include "TeinoraSim.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	const TCHAR* const Section = TEXT("WindsOfTeinora");
	const TCHAR* const ActionKeys[] = { TEXT("KeyForward"), TEXT("KeyBack"), TEXT("KeyLeft"), TEXT("KeyRight"), TEXT("KeyUp"), TEXT("KeyDown"), TEXT("KeyFast"), TEXT("KeyWorld") };

	void ReadInt(const TCHAR* Key, int32& Value, int32 Min, int32 Max)
	{
		GConfig->GetInt(Section, Key, Value, GGameUserSettingsIni);
		Value = FMath::Clamp(Value, Min, Max);
	}

	void ReadDouble(const TCHAR* Key, double& Value, double Min, double Max)
	{
		GConfig->GetDouble(Section, Key, Value, GGameUserSettingsIni);
		Value = FMath::Clamp(Value, Min, Max);
	}
}

double FTeinoraWorldSettings::GetTimeRate(int32 Index)
{
	static const double Rates[TimeRates] = { 0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 15.0, 30.0, 60.0, 120.0 };
	return Rates[FMath::Clamp(Index, 0, TimeRates - 1)];
}

void FTeinoraWorldSettings::Apply() const
{
	Teinora::Sim::WorldConditions Conditions;
	Conditions.WindSpeed = WindSpeed;
	Conditions.WindHeading = WindDirection >= 0 ? 2.0 * UE_DOUBLE_PI * WindDirection / WindDirections : -1.0;
	Conditions.WaveHeight = WaveHeight;
	Teinora::Sim::SetConditions(Conditions);
}

FTeinoraSettings::FTeinoraSettings()
{
	const FKey Defaults[] = { EKeys::W, EKeys::S, EKeys::A, EKeys::D, EKeys::SpaceBar, EKeys::LeftControl, EKeys::LeftShift, EKeys::Slash };
	for (int32 Action = 0; Action < static_cast<int32>(ETeinoraAction::Count); ++Action)
	{
		Keys[Action] = Defaults[Action];
	}
}

void FTeinoraSettings::BindKey(int32 Action, const FKey& Key)
{
	const FKey Old = Keys[Action];
	for (FKey& Held : Keys)
	{
		Held = Held == Key ? Old : Held;
	}
	Keys[Action] = Key;
}

int32 FTeinoraSettings::GetViewRadius() const
{
	static const int32 Radii[] = { 6, 10, 14 };
	return Radii[ViewDistance];
}

void FTeinoraSettings::Load()
{
	ReadInt(TEXT("Quality"), Quality, 0, 3);
	ReadInt(TEXT("RenderScale"), RenderScale, 50, 100);
	ReadInt(TEXT("AntiAliasing"), AntiAliasing, 0, 2);
	ReadInt(TEXT("GeometryDetail"), GeometryDetail, 0, 3);
	ReadInt(TEXT("ViewDistance"), ViewDistance, 0, 2);
	GConfig->GetBool(Section, TEXT("Borderless"), bBorderless, GGameUserSettingsIni);
	GConfig->GetBool(Section, TEXT("EyeFocus"), bEyeFocus, GGameUserSettingsIni);
	GConfig->GetBool(Section, TEXT("Clouds"), bClouds, GGameUserSettingsIni);
	ReadInt(TEXT("MouseSensitivity"), MouseSensitivity, 20, 300);
	GConfig->GetBool(Section, TEXT("InvertMouse"), bInvertMouse, GGameUserSettingsIni);
	ReadDouble(TEXT("WindSpeed"), World.WindSpeed, 0.0, FTeinoraWorldSettings::MaxWindSpeed);
	ReadInt(TEXT("WindDirection"), World.WindDirection, -1, FTeinoraWorldSettings::WindDirections - 1);
	ReadInt(TEXT("TimeSpeed"), World.TimeSpeed, 0, FTeinoraWorldSettings::TimeRates - 1);
	ReadDouble(TEXT("WaveHeight"), World.WaveHeight, 0.0, FTeinoraWorldSettings::MaxWaveHeight);
	for (int32 Action = 0; Action < static_cast<int32>(ETeinoraAction::Count); ++Action)
	{
		FString Name;
		if (GConfig->GetString(Section, ActionKeys[Action], Name, GGameUserSettingsIni) && FKey(*Name).IsValid())
		{
			Keys[Action] = FKey(*Name);
		}
	}
}

void FTeinoraSettings::Save() const
{
	GConfig->SetInt(Section, TEXT("Quality"), Quality, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("RenderScale"), RenderScale, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("AntiAliasing"), AntiAliasing, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("GeometryDetail"), GeometryDetail, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("ViewDistance"), ViewDistance, GGameUserSettingsIni);
	GConfig->SetBool(Section, TEXT("Borderless"), bBorderless, GGameUserSettingsIni);
	GConfig->SetBool(Section, TEXT("EyeFocus"), bEyeFocus, GGameUserSettingsIni);
	GConfig->SetBool(Section, TEXT("Clouds"), bClouds, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("MouseSensitivity"), MouseSensitivity, GGameUserSettingsIni);
	GConfig->SetBool(Section, TEXT("InvertMouse"), bInvertMouse, GGameUserSettingsIni);
	GConfig->SetDouble(Section, TEXT("WindSpeed"), World.WindSpeed, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("WindDirection"), World.WindDirection, GGameUserSettingsIni);
	GConfig->SetInt(Section, TEXT("TimeSpeed"), World.TimeSpeed, GGameUserSettingsIni);
	GConfig->SetDouble(Section, TEXT("WaveHeight"), World.WaveHeight, GGameUserSettingsIni);
	for (int32 Action = 0; Action < static_cast<int32>(ETeinoraAction::Count); ++Action)
	{
		GConfig->SetString(Section, ActionKeys[Action], *Keys[Action].ToString(), GGameUserSettingsIni);
	}
	GConfig->Flush(false, GGameUserSettingsIni);
}
