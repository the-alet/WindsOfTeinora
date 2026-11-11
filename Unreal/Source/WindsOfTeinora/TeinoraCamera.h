#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

THIRD_PARTY_INCLUDES_START
#include "Gaze.h"
#include "Render/SceneTypes.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

#include "TeinoraCamera.generated.h"

class ATeinoraController;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UTexture2D;

/** The free camera, with no body: an eye that leads and a view that follows it, as in the standalone game. */
UCLASS()
class ATeinoraCamera : public APawn
{
	GENERATED_BODY()

public:
	ATeinoraCamera();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	const Teinora::Vec3& GetEye() const { return Eye; }
	Teinora::Vec3 GetLineOfSight() const { return View.GetLineOfSight(); }

	void SetEyeFocus(bool bEnabled);

	/** How much brighter than by day to show the scene, and how much of its colour, as an eye does at night. */
	void SetExposure(double Multiplier, float Saturation);

	/** How far under water the eye is, zero above it. */
	void SetUnderwater(double Depth, const FLinearColor& Light);

	/** Feeds the magic view: how far it has faded in, and where the boxes of magic are. */
	void ShowMagic(float ViewAmount, bool bBends, UTexture2D* NearAtlas, UTexture2D* FarAtlas, const Teinora::Vec3& NearCorner, const Teinora::Vec3& NearSize,
		const Teinora::Vec3& FarCorner, const Teinora::Vec3& FarSize, double Time, const std::vector<Teinora::MagicGlow>& Glows);

	/** Feeds the rain: how hard it falls, how far the wind slants it across the view, and the time it falls by. */
	void ShowRain(float Amount, float Slant, double Time);

private:
	void ReadInput(const ATeinoraController& Player, double Dt);
	void UpdateFocus();

	UPROPERTY()
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> FocusMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MagicMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> UnderwaterMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> RainMaterial;

	Teinora::Vec3 Eye;
	Teinora::Gaze View;
	static constexpr double Speed = 10.0;
	static constexpr double FastSpeed = 50.0;
	double FlyStep = 0.0;   // scripted flight in an automated run, per frame
	double FlyTurn = 0.0;
	bool bEyeFocus = true;
};
