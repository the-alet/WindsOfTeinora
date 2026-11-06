#include "TeinoraMenu.h"

#include "Styling/CoreStyle.h"
#include "TeinoraController.h"
#include "TeinoraWorld.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FLinearColor Dim(0.0f, 0.02f, 0.05f, 0.55f);
	const FLinearColor Panel(0.03f, 0.05f, 0.09f, 0.96f);
	const FLinearColor Row(0.06f, 0.10f, 0.18f, 1.0f);
	const FLinearColor TextColor(0.92f, 0.95f, 1.0f, 1.0f);
	const FLinearColor ValueColor(0.55f, 0.78f, 1.0f, 1.0f);

	FSlateFontInfo MakeFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}

	FText ToText(const TCHAR* Value)
	{
		return FText::FromString(Value);
	}

	int32 Cycle(int32 Value, int32 Count)
	{
		return (Value + 1) % Count;
	}
}

void STeinoraMenu::Construct(const FArguments& Arguments)
{
	Owner = Arguments._Owner;

	const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	const bool bWorld = Arguments._WorldPage;
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder).BorderImage(White).BorderBackgroundColor(bWorld ? FLinearColor::Transparent : Dim).Visibility(EVisibility::HitTestInvisible)
		]
		+ SOverlay::Slot().HAlign(bWorld ? HAlign_Right : HAlign_Center).VAlign(VAlign_Center).Padding(FMargin(24.0f))
		[
			SNew(SBox).WidthOverride(bWorld ? 480.0f : 620.0f)
			[
				SNew(SBorder).BorderImage(White).BorderBackgroundColor(Panel).Padding(FMargin(24.0f))
				[
					SAssignNew(Page, SVerticalBox)
				]
			]
		]
	];

	if (bWorld)
	{
		ShowWorld();
	}
	else if (Arguments._StartOnGraphics)
	{
		ShowGraphics();
	}
	else
	{
		ShowMain();
	}
}

void STeinoraMenu::AddTitle(const FText& Title)
{
	Page->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.0f, 0.0f, 0.0f, 16.0f))
	[
		SNew(STextBlock).Text(Title).Font(MakeFont(26)).ColorAndOpacity(FSlateColor(TextColor))
	];
}

void STeinoraMenu::AddAction(const FText& Label, TFunction<void()> Action)
{
	Page->AddSlot().AutoHeight().Padding(FMargin(0.0f, 2.0f))
	[
		SNew(SButton)
		.ButtonColorAndOpacity(FSlateColor(Row))
		.ContentPadding(FMargin(16.0f, 8.0f))
		.HAlign(HAlign_Center)
		.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); })
		[
			SNew(STextBlock).Text(Label).Font(MakeFont(17)).ColorAndOpacity(FSlateColor(TextColor))
		]
	];
}

void STeinoraMenu::AddSetting(const FText& Label, TFunction<FText()> Value, TFunction<void()> Change)
{
	TWeakObjectPtr<ATeinoraController> Player = Owner;
	Page->AddSlot().AutoHeight().Padding(FMargin(0.0f, 2.0f))
	[
		SNew(SButton)
		.ButtonColorAndOpacity(FSlateColor(Row))
		.ContentPadding(FMargin(16.0f, 8.0f))
		.OnClicked_Lambda([Change, Player]()
		{
			Change();
			if (Player.IsValid())
			{
				Player->ApplySettings();
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(STextBlock).Text(Label).Font(MakeFont(17)).ColorAndOpacity(FSlateColor(TextColor))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock).Text_Lambda([Value]() { return Value(); }).Font(MakeFont(17)).ColorAndOpacity(FSlateColor(ValueColor))
			]
		]
	];
}

void STeinoraMenu::ShowMain()
{
	Page->ClearChildren();
	AddTitle(ToText(TEXT("Paused")));
	AddAction(ToText(TEXT("Settings")), [this]() { ShowGraphics(); });
	AddAction(ToText(TEXT("Exit")), [this]()
	{
		if (Owner.IsValid())
		{
			Owner->Quit();
		}
	});
}

void STeinoraMenu::AddTabs()
{
	const auto MakeTab = [this](const TCHAR* Name, TFunction<void()> Open)
	{
		return SNew(SButton)
			.ButtonColorAndOpacity(FSlateColor(Row))
			.ContentPadding(FMargin(16.0f, 8.0f))
			.HAlign(HAlign_Center)
			.OnClicked_Lambda([Open]() { Open(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(ToText(Name)).Font(MakeFont(17)).ColorAndOpacity(FSlateColor(ValueColor))
			];
	};

	Page->AddSlot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 10.0f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(0.0f, 0.0f, 2.0f, 0.0f))
		[
			MakeTab(TEXT("Graphics"), [this]() { ShowGraphics(); })
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(2.0f, 0.0f, 0.0f, 0.0f))
		[
			MakeTab(TEXT("Controls"), [this]() { ShowControls(); })
		]
	];
}

void STeinoraMenu::ShowControls()
{
	if (!Owner.IsValid())
	{
		return;
	}
	FTeinoraSettings* Settings = &Owner->GetSettings();
	ATeinoraController* Player = Owner.Get();

	Page->ClearChildren();
	AddTitle(ToText(TEXT("Settings")));
	AddTabs();

	static const TCHAR* const Names[] = { TEXT("Move Forward"), TEXT("Move Back"), TEXT("Move Left"), TEXT("Move Right"), TEXT("Move Up"), TEXT("Move Down"), TEXT("Move Fast"), TEXT("World Settings") };
	for (int32 Action = 0; Action < static_cast<int32>(ETeinoraAction::Count); ++Action)
	{
		AddSetting(ToText(Names[Action]),
			[Settings, Player, Action]()
			{
				return Player->GetRebindingAction() == Action ? ToText(TEXT("Press a key, Esc to cancel")) : Settings->Keys[Action].GetDisplayName();
			},
			[Player, Action]() { Player->StartRebinding(Action); });
	}

	AddSetting(ToText(TEXT("Mouse Sensitivity")),
		[Settings]() { return FText::FromString(FString::Printf(TEXT("%d%%"), Settings->MouseSensitivity)); },
		[Settings]() { Settings->MouseSensitivity = Settings->MouseSensitivity >= 300 ? 20 : Settings->MouseSensitivity + 20; });

	AddSetting(ToText(TEXT("Invert Mouse")),
		[Settings]() { return ToText(Settings->bInvertMouse ? TEXT("On") : TEXT("Off")); },
		[Settings]() { Settings->bInvertMouse = !Settings->bInvertMouse; });

	AddSetting(ToText(TEXT("Throw Ball")), []() { return ToText(TEXT("Hold Left Mouse")); }, []() {});
	AddSetting(ToText(TEXT("Menu")), []() { return ToText(TEXT("Esc")); }, []() {});

	AddAction(ToText(TEXT("Back")), [this]() { ShowMain(); });
}

void STeinoraMenu::ShowWorld()
{
	if (!Owner.IsValid())
	{
		return;
	}
	FTeinoraWorldSettings* World = &Owner->GetSettings().World;
	ATeinoraController* Player = Owner.Get();

	Page->ClearChildren();
	AddTitle(ToText(TEXT("World")));

	AddSetting(ToText(TEXT("Wind Speed")),
		[World]() { return FText::FromString(FString::Printf(TEXT("%.1f m/s"), World->WindSpeed)); },
		[World]() { World->WindSpeed = World->WindSpeed >= FTeinoraWorldSettings::MaxWindSpeed ? 0.0 : World->WindSpeed + FTeinoraWorldSettings::WindStep; });

	AddSetting(ToText(TEXT("Wind Direction")),
		[World]()
		{
			static const TCHAR* const Names[] = { TEXT("East"), TEXT("North-East"), TEXT("North"), TEXT("North-West"), TEXT("West"), TEXT("South-West"), TEXT("South"), TEXT("South-East") };
			return ToText(World->WindDirection >= 0 ? Names[World->WindDirection] : TEXT("Changing"));
		},
		[World]() { World->WindDirection = Cycle(World->WindDirection + 1, FTeinoraWorldSettings::WindDirections + 1) - 1; });

	AddSetting(ToText(TEXT("Time Speed")),
		[World]()
		{
			const double Rate = FTeinoraWorldSettings::GetTimeRate(World->TimeSpeed);
			return Rate > 0.0 ? FText::FromString(FString::Printf(TEXT("x%g"), Rate)) : ToText(TEXT("Stopped"));
		},
		[World]() { World->TimeSpeed = Cycle(World->TimeSpeed, FTeinoraWorldSettings::TimeRates); });

	AddSetting(ToText(TEXT("Time of Day")),
		[Player]()
		{
			const ATeinoraWorld* Place = Player->GetTeinoraWorld();
			const int32 Minutes = Place != nullptr ? static_cast<int32>(Place->GetStats().Hour * 60.0) : 0;
			return FText::FromString(FString::Printf(TEXT("%02d:%02d"), Minutes / 60, Minutes % 60));
		},
		[Player]()
		{
			if (ATeinoraWorld* Place = Player->GetTeinoraWorld())
			{
				const FTeinoraWorldStats Stats = Place->GetStats();
				Place->SetClock(FMath::Fmod(FMath::FloorToDouble(Stats.Hour * 2.0 + 0.001) / 2.0 + 0.5, 24.0), Stats.Day);
			}
		});

	AddSetting(ToText(TEXT("Day")),
		[Player]()
		{
			const ATeinoraWorld* Place = Player->GetTeinoraWorld();
			return FText::FromString(FString::Printf(TEXT("%d"), Place != nullptr ? static_cast<int32>(Place->GetStats().Day) : 1));
		},
		[Player]()
		{
			if (ATeinoraWorld* Place = Player->GetTeinoraWorld())
			{
				const FTeinoraWorldStats Stats = Place->GetStats();
				Place->SetClock(Stats.Hour, Stats.Day + 1.0);
			}
		});

	AddSetting(ToText(TEXT("Wave Height")),
		[World]() { return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(World->WaveHeight * 100.0))); },
		[World]() { World->WaveHeight = World->WaveHeight >= FTeinoraWorldSettings::MaxWaveHeight ? 0.0 : World->WaveHeight + FTeinoraWorldSettings::WaveStep; });

	AddSetting(ToText(TEXT("Cloud Cover")),
		[World]() { return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(World->CloudCover * 100.0))); },
		[World]() { World->CloudCover = World->CloudCover >= FTeinoraWorldSettings::MaxCloudCover ? 0.0 : World->CloudCover + FTeinoraWorldSettings::CloudStep; });

	AddSetting(ToText(TEXT("Reset to Defaults")), []() { return FText::GetEmpty(); }, [World]() { *World = FTeinoraWorldSettings(); });
}

void STeinoraMenu::ShowGraphics()
{
	if (!Owner.IsValid())
	{
		return;
	}
	FTeinoraSettings* Settings = &Owner->GetSettings();

	Page->ClearChildren();
	AddTitle(ToText(TEXT("Settings")));
	AddTabs();

	AddSetting(ToText(TEXT("Quality Preset")),
		[Settings]()
		{
			static const TCHAR* const Names[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Ultra") };
			return ToText(Names[Settings->Quality]);
		},
		[Settings]() { Settings->Quality = Cycle(Settings->Quality, 4); });

	AddSetting(ToText(TEXT("Window Mode")),
		[Settings]() { return ToText(Settings->bBorderless ? TEXT("Borderless") : TEXT("Windowed")); },
		[Settings]() { Settings->bBorderless = !Settings->bBorderless; });

	AddSetting(ToText(TEXT("Render Scale")),
		[Settings]() { return FText::FromString(FString::Printf(TEXT("%d%%"), Settings->RenderScale)); },
		[Settings]() { Settings->RenderScale = 50 + Cycle((Settings->RenderScale - 50) / 25, 3) * 25; });

	AddSetting(ToText(TEXT("Anti-Aliasing")),
		[Settings]()
		{
			static const TCHAR* const Names[] = { TEXT("Off"), TEXT("FXAA"), TEXT("TSR") };
			return ToText(Names[Settings->AntiAliasing]);
		},
		[Settings]() { Settings->AntiAliasing = Cycle(Settings->AntiAliasing, 3); });

	AddSetting(ToText(TEXT("Geometry Detail")),
		[Settings]()
		{
			static const TCHAR* const Names[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Ultra") };
			return ToText(Names[Settings->GeometryDetail]);
		},
		[Settings]() { Settings->GeometryDetail = Cycle(Settings->GeometryDetail, 4); });

	AddSetting(ToText(TEXT("View Distance")),
		[Settings]()
		{
			static const TCHAR* const Names[] = { TEXT("Near"), TEXT("Medium"), TEXT("Far") };
			return ToText(Names[Settings->ViewDistance]);
		},
		[Settings]() { Settings->ViewDistance = Cycle(Settings->ViewDistance, 3); });

	AddSetting(ToText(TEXT("Eye Focus")),
		[Settings]() { return ToText(Settings->bEyeFocus ? TEXT("On") : TEXT("Off")); },
		[Settings]() { Settings->bEyeFocus = !Settings->bEyeFocus; });

	AddSetting(ToText(TEXT("Clouds")),
		[Settings]() { return ToText(Settings->bClouds ? TEXT("On") : TEXT("Off")); },
		[Settings]() { Settings->bClouds = !Settings->bClouds; });

	AddAction(ToText(TEXT("Back")), [this]() { ShowMain(); });
}
