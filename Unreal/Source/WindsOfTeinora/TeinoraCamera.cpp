#include "TeinoraCamera.h"

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TeinoraController.h"
#include "TeinoraConvert.h"
#include "TeinoraWorld.h"

THIRD_PARTY_INCLUDES_START
#include "TeinoraSim.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	using Teinora::Vec3;
	namespace Sim = Teinora::Sim;

	// Unreal reports mouse travel in raw counts times this, set in BaseInput.ini.
	constexpr double AxisUnitsPerCount = 0.07;

	constexpr float FocusRadius = 0.23f;
	constexpr float FocusStrength = 0.75f;

	constexpr float DayExposureBias = -0.7f; // in stops, for the sun's intensity in TeinoraWorld.cpp
}

ATeinoraCamera::ATeinoraCamera()
{
	PrimaryActorTick.bCanEverTick = true;

	// Unreal's field of view is horizontal, at the camera's own aspect ratio. Keeping the
	// vertical one then makes it 63 degrees whatever the shape of the window.
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	const double HorizontalFov = 2.0 * FMath::Atan(FMath::Tan(Teinora::Gaze::VerticalFov * 0.5) * Camera->AspectRatio);
	Camera->SetFieldOfView(static_cast<float>(FMath::RadiansToDegrees(HorizontalFov)));
	Camera->bOverrideAspectRatioAxisConstraint = true;
	Camera->SetAspectRatioAxisConstraint(AspectRatio_MaintainYFOV);
	Camera->bConstrainAspectRatio = false;
	Camera->bUsePawnControlRotation = false;
	RootComponent = Camera;

	// Exposure follows the hour. Left to itself it would turn a moonlit night back into day.
	FPostProcessSettings& Post = Camera->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = AEM_Manual;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = false;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = DayExposureBias;
}

void ATeinoraCamera::BeginPlay()
{
	Super::BeginPlay();

	Eye = Sim::FindLandSpawn(Sim::WorldSeed) + Vec3{ 0.0, 0.0, 14.0 };
	double Yaw = Sim::FindInlandYaw(Eye, Sim::WorldSeed);
	double Pitch = -0.12;
	double GazeYaw = 0.0;
	double GazePitch = 0.0;

	// Automated runs give their own viewpoint, in the terms of the standalone game's --camera.
	FString Text;
	if (FParse::Value(FCommandLine::Get(), TEXT("TeinoraCamera="), Text, false))
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 5)
		{
			Eye = { FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]) };
			Yaw = FCString::Atod(*Parts[3]);
			Pitch = FCString::Atod(*Parts[4]);
		}
	}
	double GroundHeight = -1.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("TeinoraGround="), GroundHeight) && GroundHeight >= 0.0)
	{
		Eye.Z = Sim::TerrainHeight(Eye.X, Eye.Y, Sim::WorldSeed) + GroundHeight;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("TeinoraGaze="), Text, false))
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 2)
		{
			GazeYaw = FCString::Atod(*Parts[0]);
			GazePitch = FCString::Atod(*Parts[1]);
		}
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("TeinoraFly="), Text, false))
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 2)
		{
			FlyStep = FCString::Atod(*Parts[0]);
			FlyTurn = FCString::Atod(*Parts[1]);
		}
	}
	View.Aim(Yaw, Pitch, GazeYaw, GazePitch);

	// Magic first, so that the eye's focus falls on it like on anything else.
	if constexpr (Sim::bMagicEnabled)
	{
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Magic.M_Magic")))
		{
			MagicMaterial = UMaterialInstanceDynamic::Create(Base, this);
			Camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, MagicMaterial));
		}
	}

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Underwater.M_Underwater")))
	{
		UnderwaterMaterial = UMaterialInstanceDynamic::Create(Base, this);
		Camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(0.0f, UnderwaterMaterial));
	}

	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Focus.M_Focus")))
	{
		FocusMaterial = UMaterialInstanceDynamic::Create(Base, this);
		Camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, FocusMaterial));
		Camera->PostProcessBlendWeight = 1.0f;
	}
}

void ATeinoraCamera::SetEyeFocus(bool bEnabled)
{
	bEyeFocus = bEnabled;
}

void ATeinoraCamera::SetExposure(double Multiplier)
{
	Camera->PostProcessSettings.AutoExposureBias = DayExposureBias + static_cast<float>(FMath::Log2(Multiplier));
}

void ATeinoraCamera::SetUnderwater(double Depth, const FLinearColor& Light)
{
	if (UnderwaterMaterial == nullptr)
	{
		return;
	}

	for (FWeightedBlendable& Item : Camera->PostProcessSettings.WeightedBlendables.Array)
	{
		if (Item.Object == UnderwaterMaterial)
		{
			Item.Weight = Depth > 0.0 ? 1.0f : 0.0f; // nothing is drawn at no weight
		}
	}
	if (Depth <= 0.0)
	{
		return;
	}

	const auto ToColor = [](const Vec3& Value)
	{
		return FLinearColor(static_cast<float>(Value.X), static_cast<float>(Value.Y), static_cast<float>(Value.Z));
	};

	FVector2D Size(16.0, 9.0);
	if (GEngine->GameViewport != nullptr)
	{
		GEngine->GameViewport->GetViewportSize(Size);
	}
	const double TanY = FMath::Tan(Teinora::Gaze::VerticalFov * 0.5);
	const double TanX = TanY * Size.X / FMath::Max(Size.Y, 1.0);

	// The rays run in the world's own axes and metres, as the magic's do.
	const Vec3 Forward = View.GetForward();
	const Vec3 Right{ FMath::Sin(View.Yaw), -FMath::Cos(View.Yaw), 0.0 };
	const Vec3 Up = Teinora::Cross(Right, Forward);
	UnderwaterMaterial->SetVectorParameterValue(TEXT("ViewForward"), ToColor(Forward));
	UnderwaterMaterial->SetVectorParameterValue(TEXT("ViewRight"), ToColor(Right * TanX));
	UnderwaterMaterial->SetVectorParameterValue(TEXT("ViewUp"), ToColor(Up * TanY));

	// Less and less light comes down this far, and the haze it lights goes with it.
	UnderwaterMaterial->SetScalarParameterValue(TEXT("WaterAbove"), static_cast<float>(Depth));
	UnderwaterMaterial->SetVectorParameterValue(TEXT("WaterHaze"), FLinearColor(0.03f, 0.17f, 0.19f) * Light * FMath::Exp(-static_cast<float>(Depth) * 0.09f));
}

void ATeinoraCamera::ShowMagic(float ViewAmount, bool bBends, UTexture2D* NearAtlas, UTexture2D* FarAtlas, const Vec3& NearCorner, const Vec3& NearSize,
	const Vec3& FarCorner, const Vec3& FarSize, double Time)
{
	if (MagicMaterial == nullptr)
	{
		return;
	}

	const auto ToColor = [](const Vec3& Value)
	{
		return FLinearColor(static_cast<float>(Value.X), static_cast<float>(Value.Y), static_cast<float>(Value.Z));
	};

	FVector2D Size(16.0, 9.0);
	if (GEngine->GameViewport != nullptr)
	{
		GEngine->GameViewport->GetViewportSize(Size);
	}
	const double TanY = FMath::Tan(Teinora::Gaze::VerticalFov * 0.5);
	const double TanX = TanY * Size.X / FMath::Max(Size.Y, 1.0);

	// The shader rays run in the world's own axes and metres, relative to the eye.
	const Vec3 Forward = View.GetForward();
	const Vec3 Right{ FMath::Sin(View.Yaw), -FMath::Cos(View.Yaw), 0.0 };
	const Vec3 Up = Teinora::Cross(Right, Forward);

	MagicMaterial->SetScalarParameterValue(TEXT("MagicView"), ViewAmount);
	MagicMaterial->SetScalarParameterValue(TEXT("MagicBends"), bBends ? 1.0f : 0.0f);
	MagicMaterial->SetScalarParameterValue(TEXT("MagicTime"), static_cast<float>(Time));
	MagicMaterial->SetTextureParameterValue(TEXT("NearAtlas"), NearAtlas);
	MagicMaterial->SetTextureParameterValue(TEXT("FarAtlas"), FarAtlas);
	MagicMaterial->SetVectorParameterValue(TEXT("NearCorner"), ToColor(NearCorner - Eye));
	MagicMaterial->SetVectorParameterValue(TEXT("NearSize"), ToColor(NearSize));
	MagicMaterial->SetVectorParameterValue(TEXT("FarCorner"), ToColor(FarCorner - Eye));
	MagicMaterial->SetVectorParameterValue(TEXT("FarSize"), ToColor(FarSize));
	MagicMaterial->SetVectorParameterValue(TEXT("ViewForward"), ToColor(Forward));
	MagicMaterial->SetVectorParameterValue(TEXT("ViewRight"), ToColor(Right * TanX));
	MagicMaterial->SetVectorParameterValue(TEXT("ViewUp"), ToColor(Up * TanY));
}

void ATeinoraCamera::ReadInput(const ATeinoraController& Player, double Dt)
{
	double MouseX = 0.0;
	double MouseY = 0.0;
	Player.GetInputMouseDelta(MouseX, MouseY);
	MouseX /= AxisUnitsPerCount;
	MouseY /= AxisUnitsPerCount;

	const FTeinoraSettings& Controls = Player.GetSettings();
	const double Rate = Controls.MouseSensitivity / 100.0;
	const double TurnX = -MouseX * Rate;
	const double TurnY = MouseY * Rate * (Controls.bInvertMouse ? -1.0 : 1.0);
	if (bEyeFocus)
	{
		View.Turn(TurnX, TurnY, Dt);
	}
	else
	{
		View.TurnCamera(TurnX, TurnY);
	}

	const Vec3 Forward = View.GetForward();
	const Vec3 Right{ FMath::Sin(View.Yaw), -FMath::Cos(View.Yaw), 0.0 };
	const auto Held = [&Player, &Controls](ETeinoraAction Action) { return Player.IsInputKeyDown(Controls.GetKey(Action)); };

	Vec3 Move;
	if (Held(ETeinoraAction::Forward)) { Move += Forward; }
	if (Held(ETeinoraAction::Back)) { Move -= Forward; }
	if (Held(ETeinoraAction::Right)) { Move += Right; }
	if (Held(ETeinoraAction::Left)) { Move -= Right; }
	if (Held(ETeinoraAction::Up)) { Move += Vec3{ 0.0, 0.0, 1.0 }; }
	if (Held(ETeinoraAction::Down)) { Move -= Vec3{ 0.0, 0.0, 1.0 }; }

	Eye += Teinora::Normalize(Move) * ((Held(ETeinoraAction::Fast) ? FastSpeed : Speed) * Dt);

	// The camera may fly anywhere and dive under water, but not go through the ground.
	Eye.Z = FMath::Max(Eye.Z, Sim::TerrainHeight(Eye.X, Eye.Y, Sim::WorldSeed) + 1.2);
}

void ATeinoraCamera::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const ATeinoraController* Player = Cast<ATeinoraController>(GetController());
	if (Player != nullptr && !Player->IsMenuOpen())
	{
		if (Player->IsAutomated())
		{
			const ATeinoraWorld* World = Player->GetTeinoraWorld();
			if (World != nullptr && World->IsSettled())
			{
				View.Yaw += FlyTurn;
				Eye += View.GetForward() * FlyStep;
			}
		}
		else
		{
			ReadInput(*Player, DeltaSeconds);
		}
	}

	SetActorLocationAndRotation(TeinoraConvert::ToUnreal(Eye),
		FRotator(FMath::RadiansToDegrees(View.Pitch), -FMath::RadiansToDegrees(View.Yaw), 0.0));
	UpdateFocus();
}

void ATeinoraCamera::UpdateFocus()
{
	if (FocusMaterial == nullptr)
	{
		return;
	}

	FVector2D Size(16.0, 9.0);
	if (GEngine->GameViewport != nullptr)
	{
		GEngine->GameViewport->GetViewportSize(Size);
	}
	const double Aspect = Size.X / FMath::Max(Size.Y, 1.0);

	// The line of sight as a point on the screen.
	const Vec3 Forward = View.GetForward();
	const Vec3 Right{ FMath::Sin(View.Yaw), -FMath::Cos(View.Yaw), 0.0 };
	const Vec3 Up = Teinora::Cross(Right, Forward);
	const Vec3 Sight = View.GetLineOfSight();
	const double Depth = Teinora::Dot(Sight, Forward);
	const double Focal = 1.0 / FMath::Tan(Teinora::Gaze::VerticalFov * 0.5);
	const double U = 0.5 + 0.5 * Teinora::Dot(Sight, Right) / Depth * Focal / Aspect;
	const double V = 0.5 - 0.5 * Teinora::Dot(Sight, Up) / Depth * Focal;

	FocusMaterial->SetVectorParameterValue(TEXT("FocusCenter"), FLinearColor(static_cast<float>(U), static_cast<float>(V), 0.0f));
	FocusMaterial->SetScalarParameterValue(TEXT("FocusRadius"), FocusRadius);
	FocusMaterial->SetScalarParameterValue(TEXT("FocusStrength"), bEyeFocus ? FocusStrength : 0.0f);
}
