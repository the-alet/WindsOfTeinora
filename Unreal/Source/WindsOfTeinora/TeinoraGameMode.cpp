#include "TeinoraGameMode.h"

#include "Engine/World.h"
#include "TeinoraCamera.h"
#include "TeinoraController.h"
#include "TeinoraHud.h"
#include "TeinoraWorld.h"

ATeinoraGameMode::ATeinoraGameMode()
{
	DefaultPawnClass = ATeinoraCamera::StaticClass();
	PlayerControllerClass = ATeinoraController::StaticClass();
	HUDClass = ATeinoraHud::StaticClass();
}

void ATeinoraGameMode::InitGameState()
{
	Super::InitGameState();
	TeinoraWorld = GetWorld()->SpawnActor<ATeinoraWorld>();
}
