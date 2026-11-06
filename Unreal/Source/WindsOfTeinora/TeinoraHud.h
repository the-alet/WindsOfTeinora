#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "TeinoraHud.generated.h"

/** The same readout and key hints as the standalone game. */
UCLASS()
class ATeinoraHud : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	double SmoothedFps = 0.0;
};
