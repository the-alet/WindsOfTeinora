#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class ATeinoraController;
class SVerticalBox;

/**
 * The Esc menu: a main page with Settings and Exit, and settings with a tab each for graphics and
 * controls. And the world menu on its own key, at the side, with the world still running behind it.
 */
class STeinoraMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(STeinoraMenu) {}
		SLATE_ARGUMENT(ATeinoraController*, Owner)
		SLATE_ARGUMENT(bool, StartOnGraphics)
		SLATE_ARGUMENT(bool, WorldPage)
	SLATE_END_ARGS()

	void Construct(const FArguments& Arguments);

private:
	void ShowMain();
	void ShowGraphics();
	void ShowControls();
	void ShowWorld();
	void AddTabs();
	void AddTitle(const FText& Title);
	void AddAction(const FText& Label, TFunction<void()> Action);
	void AddSetting(const FText& Label, TFunction<FText()> Value, TFunction<void()> Change);

	TWeakObjectPtr<ATeinoraController> Owner;
	TSharedPtr<SVerticalBox> Page;
};
