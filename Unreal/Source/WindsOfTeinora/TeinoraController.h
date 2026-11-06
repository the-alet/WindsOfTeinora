#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "TeinoraSettings.h"
#include "TeinoraController.generated.h"

class ATeinoraCamera;
class ATeinoraWorld;
class STeinoraMenu;

/** Throwing and the Esc menu. An automated run leaves the mouse alone. */
UCLASS()
class ATeinoraController : public APlayerController
{
	GENERATED_BODY()

public:
	ATeinoraController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void PlayerTick(float DeltaTime) override;

	bool IsMenuOpen() const { return Menu.IsValid(); }
	bool IsAutomated() const { return bAutomated; }
	ATeinoraWorld* GetTeinoraWorld() const;

	/** Share of the strongest throw while the button is held, otherwise below zero. */
	double GetThrowCharge() const;

	FTeinoraSettings& GetSettings() { return Settings; }
	const FTeinoraSettings& GetSettings() const { return Settings; }

	/** Makes the next key pressed the key of an action, which Esc gives up. */
	void StartRebinding(int32 Action) { RebindingAction = Action; }
	int32 GetRebindingAction() const { return RebindingAction; }

	/** Puts the settings into effect and saves them. */
	void ApplySettings();

	void CloseMenu();
	void Quit();

private:
	void OpenMenu(bool bGraphicsPage = false, bool bWorldPage = false);
	bool WasWorldKeyPressed() const;

	TSharedPtr<STeinoraMenu> Menu;
	bool bWorldMenu = false; // the one open is the world menu, with the world running behind it
	FTeinoraSettings Settings;
	bool bAutomated = false;
	int32 RebindingAction = INDEX_NONE;
	double ThrowHeld = -1.0; // seconds the button has been down for, below zero while it is up
};
