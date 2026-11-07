#include "TeinoraHud.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Runtime/Launch/Resources/Version.h"
#include "TeinoraCamera.h"
#include "TeinoraController.h"
#include "TeinoraWorld.h"

THIRD_PARTY_INCLUDES_START
#include "TeinoraSim.h"
THIRD_PARTY_INCLUDES_END

namespace Sim = Teinora::Sim;

void ATeinoraHud::DrawHUD()
{
	Super::DrawHUD();

	ATeinoraController* Player = Cast<ATeinoraController>(GetOwningPlayerController());
	const ATeinoraCamera* Eye = Player != nullptr ? Cast<ATeinoraCamera>(Player->GetPawn()) : nullptr;
	const ATeinoraWorld* World = Player != nullptr ? Player->GetTeinoraWorld() : nullptr;
	if (Eye == nullptr || World == nullptr || Canvas == nullptr)
	{
		return;
	}

	const double Delta = FApp::GetDeltaTime();
	const double Fps = 1.0 / FMath::Max(Delta, 1e-4);
	SmoothedFps = SmoothedFps == 0.0 ? Fps : SmoothedFps * 0.95 + Fps * 0.05;

	const FTeinoraWorldStats Stats = World->GetStats();
	const int32 Hours = static_cast<int32>(Stats.Hour);
	const int32 Minutes = static_cast<int32>((Stats.Hour - Hours) * 60.0);
	const Teinora::Vec3& Position = Eye->GetEye();

	TArray<FString> Lines = {
		FString::Printf(TEXT("Unreal Engine %d.%d | %s | %s | %.0f FPS"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, GDynamicRHI->GetName(), *GRHIAdapterName, SmoothedFps),
		FString::Printf(TEXT("Day %.0f   Time %02d:%02d   Wind %.1f m/s"), Stats.Day, Hours, Minutes, Stats.WindSpeed),
		FString::Printf(TEXT("Position %.0f, %.0f   Altitude %.0f m   Bodies %d, awake %d, contacts %d"), Position.X, Position.Y, Position.Z, Stats.Bodies, Stats.Awake, Stats.Contacts),
	};
	if (Stats.bMagic)
	{
		Lines.Add(FString::Printf(TEXT("Magic density %.3f kg/m3: earth %.3f, water %.3f, air %.3f, nature %.3f, fire %.3f, neutral %.3f%s"), Stats.MagicEarth + Stats.MagicWater + Stats.MagicAir + Stats.MagicNature + Stats.MagicFire + Stats.MagicNeutral,
			Stats.MagicEarth, Stats.MagicWater, Stats.MagicAir, Stats.MagicNature, Stats.MagicFire, Stats.MagicNeutral, Stats.bMagicView ? TEXT("   Magic view") : TEXT("")));
		Lines.Add(FString::Printf(TEXT("Cloud above %.0f m deep: water %.3f, air %.3f, passing at %.0f m/s, rubbing %.0f Pa, lightning %.3f of %.2f"), Stats.CloudDepth, Stats.CloudWater, Stats.CloudAir, Stats.CloudSpeed, Stats.CloudRubbing, Stats.CloudLightning, Sim::MagicBreakdown));
	}

	const FLinearColor Backdrop(0.0f, 0.02f, 0.05f, 0.5f);
	const FLinearColor MagicText(0.70f, 1.0f, 0.42f, 1.0f);
	const FTeinoraSettings& Controls = Player->GetSettings();
	const auto Name = [&Controls](ETeinoraAction Action)
	{
		const FKey& Key = Controls.GetKey(Action);
		return Key == EKeys::Slash ? FString(TEXT("/")) : Key.GetDisplayName().ToString();
	};
	const FString Hints = FString::Printf(TEXT("%s %s %s %s move   %s / %s up, down   %s fast   Hold LMB throw ball   1 fire  2 crate  3 barrel  4 ball   %s%s world   Esc menu"),
		*Name(ETeinoraAction::Forward), *Name(ETeinoraAction::Left), *Name(ETeinoraAction::Back), *Name(ETeinoraAction::Right),
		*Name(ETeinoraAction::Up), *Name(ETeinoraAction::Down), *Name(ETeinoraAction::Fast), Stats.bMagic ? TEXT("M magic   ") : TEXT(""), *Name(ETeinoraAction::WorldMenu));
	UFont* Font = GEngine->GetMediumFont();

	float Widest = 0.0f;
	float LineHeight = 0.0f;
	for (const FString& Line : Lines)
	{
		float Width = 0.0f;
		GetTextSize(Line, Width, LineHeight, Font);
		Widest = FMath::Max(Widest, Width);
	}

	DrawRect(Backdrop, 10.0f, 10.0f, Widest + 16.0f, LineHeight * Lines.Num() + 16.0f);
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		DrawText(Lines[Index], Index >= 3 ? MagicText : FLinearColor::White, 18.0f, 18.0f + LineHeight * Index, Font);
	}

	const double Charge = Player->GetThrowCharge();
	if (Charge >= 0.0)
	{
		const float BarWidth = 280.0f;
		const float BarX = (Canvas->SizeX - BarWidth) * 0.5f;
		const float BarY = Canvas->SizeY - 70.0f;
		DrawRect(Backdrop, BarX - 2.0f, BarY - 2.0f, BarWidth + 4.0f, 14.0f);
		DrawRect(FLinearColor(1.0f, 0.82f, 0.35f, 1.0f), BarX, BarY, BarWidth * static_cast<float>(Charge), 10.0f);
	}

	float HintWidth = 0.0f;
	GetTextSize(Hints, HintWidth, LineHeight, Font);
	DrawRect(Backdrop, 10.0f, Canvas->SizeY - LineHeight - 26.0f, HintWidth + 16.0f, LineHeight + 16.0f);
	DrawText(Hints, FLinearColor(0.82f, 0.88f, 0.95f, 1.0f), 18.0f, Canvas->SizeY - LineHeight - 18.0f, Font);
}
