#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TeinoraGameMode.generated.h"

class ATeinoraWorld;

/** Needs nothing from the map: everything in the world is made at run time. */
UCLASS()
class ATeinoraGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATeinoraGameMode();

	virtual void InitGameState() override;

	ATeinoraWorld* GetTeinoraWorld() const { return TeinoraWorld; }

private:
	UPROPERTY()
	TObjectPtr<ATeinoraWorld> TeinoraWorld;
};
