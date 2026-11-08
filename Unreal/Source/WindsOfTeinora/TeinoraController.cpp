#include "TeinoraController.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TeinoraCamera.h"
#include "TeinoraGameMode.h"
#include "TeinoraMenu.h"
#include "TeinoraWorld.h"

THIRD_PARTY_INCLUDES_START
#include "TeinoraSim.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	void SetConsoleValue(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, ECVF_SetByGameSetting);
		}
	}
}

ATeinoraController::ATeinoraController()
{
	bShowMouseCursor = false;
}

void ATeinoraController::BeginPlay()
{
	Super::BeginPlay();

	int32 Frames = 0;
	bAutomated = FParse::Value(FCommandLine::Get(), TEXT("TeinoraFrames="), Frames) && Frames > 0;

	Settings.Load();
	ApplySettings();

	if (!bAutomated)
	{
		SetInputMode(FInputModeGameOnly());
	}

	FString Page;
	if (FParse::Value(FCommandLine::Get(), TEXT("TeinoraMenu="), Page))
	{
		OpenMenu(Page == TEXT("settings"), Page == TEXT("world"));
	}
}

void ATeinoraController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Menu.IsValid() && GEngine->GameViewport != nullptr)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(Menu.ToSharedRef());
	}
	Menu.Reset();
	Super::EndPlay(Reason);
}

ATeinoraWorld* ATeinoraController::GetTeinoraWorld() const
{
	const ATeinoraGameMode* Mode = GetWorld()->GetAuthGameMode<ATeinoraGameMode>();
	return Mode != nullptr ? Mode->GetTeinoraWorld() : nullptr;
}

void ATeinoraController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (bAutomated)
	{
		return;
	}

	// While a row of the menu waits for its key, every key goes to it.
	if (RebindingAction != INDEX_NONE)
	{
		TArray<FKey> AllKeys;
		EKeys::GetAllKeys(AllKeys);
		for (const FKey& Key : AllKeys)
		{
			if (Key.IsDigital() && !Key.IsMouseButton() && !Key.IsGamepadKey() && WasInputKeyJustPressed(Key))
			{
				if (Key != EKeys::Escape)
				{
					Settings.BindKey(RebindingAction, Key);
					ApplySettings();
				}
				RebindingAction = INDEX_NONE;
				break;
			}
		}
		return;
	}

	if (WasInputKeyJustPressed(EKeys::Escape))
	{
		if (IsMenuOpen())
		{
			CloseMenu();
		}
		else
		{
			OpenMenu();
		}
	}
	else if (WasWorldKeyPressed())
	{
		if (!IsMenuOpen())
		{
			OpenMenu(false, true);
		}
		else if (bWorldMenu)
		{
			CloseMenu();
		}
	}

	if (IsMenuOpen())
	{
		ThrowHeld = -1.0;
		return;
	}

	if (WasInputKeyJustPressed(EKeys::M))
	{
		if (ATeinoraWorld* World = GetTeinoraWorld())
		{
			World->ToggleMagicView();
		}
	}

	// Time goes faster or slower a step at a time, and stays so.
	const int32 TimeStep = (WasInputKeyJustPressed(EKeys::Add) || WasInputKeyJustPressed(EKeys::Equals) ? 1 : 0)
		- (WasInputKeyJustPressed(EKeys::Subtract) || WasInputKeyJustPressed(EKeys::Hyphen) ? 1 : 0);
	const int32 WantedTime = FMath::Clamp(Settings.World.TimeSpeed + TimeStep, 0, FTeinoraWorldSettings::TimeRates - 1);
	if (WantedTime != Settings.World.TimeSpeed)
	{
		Settings.World.TimeSpeed = WantedTime;
		Settings.Save();
		if (ATeinoraWorld* World = GetTeinoraWorld())
		{
			World->SetTimeRate(FTeinoraWorldSettings::GetTimeRate(WantedTime));
		}
	}

	// What can be set down where the eye rests.
	const FKey PlaceKeys[] ={ EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(PlaceKeys); ++Index)
	{
		const ATeinoraCamera* Eye = Cast<ATeinoraCamera>(GetPawn());
		ATeinoraWorld* World = GetTeinoraWorld();
		if (WasInputKeyJustPressed(PlaceKeys[Index]) && Eye != nullptr && World != nullptr)
		{
			World->PlaceByKey(Index + 1, Eye->GetEye(), Eye->GetLineOfSight());
		}
	}

	// The longer the button stays down, the harder the ball goes when it comes up.
	if (WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		ThrowHeld = 0.0;
	}
	else if (ThrowHeld >= 0.0)
	{
		ThrowHeld += DeltaTime;
	}
	if (ThrowHeld >= 0.0 && !IsInputKeyDown(EKeys::LeftMouseButton))
	{
		const ATeinoraCamera* Eye = Cast<ATeinoraCamera>(GetPawn());
		ATeinoraWorld* World = GetTeinoraWorld();
		if (Eye != nullptr && World != nullptr)
		{
			World->ThrowBall(Eye->GetEye(), Eye->GetLineOfSight(), Teinora::Sim::ThrowSpeedAfterHold(ThrowHeld));
		}
		ThrowHeld = -1.0;
	}
}

double ATeinoraController::GetThrowCharge() const
{
	return ThrowHeld >= 0.0 ? Teinora::Sim::ThrowSpeedAfterHold(ThrowHeld) / Teinora::Sim::MaxThrowSpeed : -1.0;
}

bool ATeinoraController::WasWorldKeyPressed() const
{
	// Unreal names some keys by what the keyboard's layout prints on them, and on a Russian
	// keyboard the key beside right shift is a full stop.
	const FKey& Key = Settings.GetKey(ETeinoraAction::WorldMenu);
	return WasInputKeyJustPressed(Key) || (Key == EKeys::Slash && WasInputKeyJustPressed(EKeys::Period));
}

void ATeinoraController::OpenMenu(bool bGraphicsPage, bool bWorldPage)
{
	bWorldMenu = bWorldPage;
	Menu = SNew(STeinoraMenu).Owner(this).StartOnGraphics(bGraphicsPage).WorldPage(bWorldPage);
	GEngine->GameViewport->AddViewportWidgetContent(Menu.ToSharedRef(), 10);

	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(Mode);
	SetShowMouseCursor(true);

	if (ATeinoraWorld* World = GetTeinoraWorld())
	{
		World->SetPaused(!bWorldPage);
	}
}

void ATeinoraController::CloseMenu()
{
	GEngine->GameViewport->RemoveViewportWidgetContent(Menu.ToSharedRef());
	Menu.Reset();

	SetInputMode(FInputModeGameOnly());
	SetShowMouseCursor(false);

	if (ATeinoraWorld* World = GetTeinoraWorld())
	{
		World->SetPaused(false);
	}
}

void ATeinoraController::ApplySettings()
{
	Settings.Save();
	Settings.World.Apply();

	SetConsoleValue(TEXT("r.ScreenPercentage"), Settings.RenderScale);

	static const int32 AntiAliasingMethods[] = { 0, 1, 4 }; // none, FXAA, TSR
	SetConsoleValue(TEXT("r.AntiAliasingMethod"), AntiAliasingMethods[Settings.AntiAliasing]);

	if (UGameUserSettings* User = GEngine->GetGameUserSettings())
	{
		User->SetOverallScalabilityLevel(Settings.Quality);
		if (!bAutomated)
		{
			User->SetFullscreenMode(Settings.bBorderless ? EWindowMode::WindowedFullscreen : EWindowMode::Windowed);
		}
		User->ApplySettings(false);
	}

	if (ATeinoraCamera* Eye = Cast<ATeinoraCamera>(GetPawn()))
	{
		Eye->SetEyeFocus(Settings.bEyeFocus);
	}
	if (ATeinoraWorld* World = GetTeinoraWorld())
	{
		World->SetView(Settings.GetViewRadius(), Settings.GeometryDetail, Settings.bClouds, Settings.World.CloudCover);
		// An automated run may ask for a speed of time of its own, which is not saved.
		int32 TimeStep = Settings.World.TimeSpeed;
		FParse::Value(FCommandLine::Get(), TEXT("TeinoraTimeStep="), TimeStep);
		World->SetTimeRate(FTeinoraWorldSettings::GetTimeRate(TimeStep));
	}
}

void ATeinoraController::Quit()
{
	ConsoleCommand(TEXT("quit"));
}
