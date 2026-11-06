#include "Menu.h"

#include "Platform/Window.h"
#include "Render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Teinora
{
	namespace
	{
		constexpr Float4 Dim{ 0.02f, 0.03f, 0.05f, 0.62f };
		constexpr Float4 Panel{ 0.07f, 0.09f, 0.13f, 0.92f };
		constexpr Float4 Highlight{ 0.20f, 0.36f, 0.52f, 1.0f };
		constexpr Float4 TextColor{ 0.93f, 0.95f, 0.98f, 1.0f };
		constexpr Float4 ValueColor{ 0.62f, 0.82f, 1.0f, 1.0f };
		constexpr Float4 HintColor{ 0.95f, 0.78f, 0.42f, 1.0f };

		enum class ERow
		{
			Settings,
			Exit,
			Tabs,
			Preset,
			Backend,
			WindowMode,
			RenderScale,
			Upscaling,
			LowLatency,
			RayTracing,
			Shadows,
			GeometryDetail,
			ViewDistance,
			EyeFocus,
			AntiAliasing,
			Clouds,
			MoveForward, // these eight are in the order of EAction
			MoveBack,
			MoveLeft,
			MoveRight,
			MoveUp,
			MoveDown,
			MoveFast,
			OpenWorldMenu,
			MouseSensitivity,
			InvertMouse,
			ThrowBall,
			OpenMenu,
			WindSpeed,
			WindDirection,
			TimeSpeed,
			TimeOfDay,
			DayNumber,
			WaveHeight,
			CloudCover,
			ResetWorld,
			Back
		};

		bool IsKeyRow(ERow Row)
		{
			return Row >= ERow::MoveForward && Row <= ERow::OpenWorldMenu;
		}

		EAction GetAction(ERow Row)
		{
			return static_cast<EAction>(static_cast<int>(Row) - static_cast<int>(ERow::MoveForward));
		}

		const char* OnOff(bool bValue)
		{
			return bValue ? "On" : "Off";
		}

		std::string FormatNumber(const char* Pattern, double Value)
		{
			char Text[32];
			std::snprintf(Text, sizeof(Text), Pattern, Value);
			return Text;
		}

		std::string GetValue(ERow Row, const GameSettings& Settings, const MenuContext& Context, const WorldClock& Clock)
		{
			static const char* const Directions[] = { "East", "North-East", "North", "North-West", "West", "South-West", "South", "South-East" };
			static const char* const Upscalers[] = { "Off", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance" };
			static const char* const Latency[] = { "Off", "On", "On + Boost" };
			static const char* const Traced[] = { "Off", "Shadows", "Shadows + Reflections" };
			const bool bUpscaling = Context.bHasUpscaling && Settings.Render.Upscaling != 0;
			const bool bTracing = Context.bHasRayTracing && Settings.Render.RayTracing != 0;

			static const char* const Shadows[] = { "Off", "Low", "Medium", "High" };
			static const char* const Levels[] = { "Low", "Medium", "High", "Ultra" };
			static const char* const Distances[] = { "Near", "Medium", "Far" };

			if (IsKeyRow(Row))
			{
				return GetKeyName(Settings.Controls.Get(GetAction(Row)));
			}

			switch (Row)
			{
			case ERow::Preset:
			{
				const int Preset = Settings.GetPreset();
				return Preset >= 0 ? Levels[Preset] : "Custom";
			}
			case ERow::Backend:       return Rhi::GetBackendName(Settings.Backend);
			case ERow::WindowMode:    return Settings.bBorderless ? "Borderless" : "Windowed";
			case ERow::RenderScale:   return bUpscaling ? "Set by DLSS" : std::to_string(Settings.Render.RenderScale) + "%";
			case ERow::Upscaling:     return Context.bHasUpscaling ? Upscalers[Settings.Render.Upscaling] : "Not available";
			case ERow::LowLatency:    return Context.bHasLowLatency ? Latency[Settings.Render.LowLatency] : "Not available";
			case ERow::RayTracing:    return Context.bHasRayTracing ? Traced[Settings.Render.RayTracing] : "Not available";
			case ERow::Shadows:       return bTracing ? "Ray traced" : Shadows[Settings.Render.ShadowQuality];
			case ERow::GeometryDetail: return Levels[Settings.Render.GeometryDetail];
			case ERow::ViewDistance:  return Distances[Settings.ViewDistance];
			case ERow::EyeFocus:      return OnOff(Settings.Render.bEyeFocus);
			case ERow::AntiAliasing:  return bUpscaling ? "DLSS" : (Settings.Render.bFxaa ? "FXAA" : "Off");
			case ERow::Clouds:        return OnOff(Settings.Render.bClouds);
			case ERow::MouseSensitivity: return std::to_string(Settings.Controls.MouseSensitivity) + "%";
			case ERow::InvertMouse:   return OnOff(Settings.Controls.bInvertMouse);
			case ERow::ThrowBall:     return "Hold Left Mouse";
			case ERow::OpenMenu:      return "Esc";
			case ERow::WindSpeed:     return FormatNumber("%.1f m/s", Settings.World.WindSpeed);
			case ERow::WindDirection: return Settings.World.WindDirection >= 0 ? Directions[Settings.World.WindDirection] : "Changing";
			case ERow::TimeSpeed:
			{
				const double Rate = WorldSettings::GetTimeRate(Settings.World.TimeSpeed);
				return Rate > 0.0 ? FormatNumber("x%g", Rate) : "Stopped";
			}
			case ERow::TimeOfDay:
			{
				char Text[16];
				const int Minutes = static_cast<int>(Clock.Hour * 60.0);
				std::snprintf(Text, sizeof(Text), "%02d:%02d", Minutes / 60, Minutes % 60);
				return Text;
			}
			case ERow::DayNumber:     return std::to_string(static_cast<int>(Clock.Day));
			case ERow::WaveHeight:    return std::to_string(static_cast<int>(std::lround(Settings.World.WaveHeight * 100.0))) + "%";
			case ERow::CloudCover:    return std::to_string(static_cast<int>(std::lround(Settings.World.CloudCover * 100.0))) + "%";
			default:                  return {};
			}
		}

		const char* GetLabel(ERow Row)
		{
			switch (Row)
			{
			case ERow::Settings:      return "Settings";
			case ERow::Exit:          return "Exit";
			case ERow::Preset:        return "Quality Preset";
			case ERow::GeometryDetail: return "Geometry Detail";
			case ERow::Backend:       return "Graphics API";
			case ERow::WindowMode:    return "Window Mode";
			case ERow::RenderScale:   return "Render Scale";
			case ERow::Upscaling:     return "Upscaling";
			case ERow::LowLatency:    return "NVIDIA Reflex";
			case ERow::RayTracing:    return "Ray Tracing";
			case ERow::Shadows:       return "Shadows";
			case ERow::ViewDistance:  return "View Distance";
			case ERow::EyeFocus:      return "Eye Focus";
			case ERow::AntiAliasing:  return "Anti-Aliasing";
			case ERow::Clouds:        return "Clouds";
			case ERow::MoveForward:   return "Move Forward";
			case ERow::MoveBack:      return "Move Back";
			case ERow::MoveLeft:      return "Move Left";
			case ERow::MoveRight:     return "Move Right";
			case ERow::MoveUp:        return "Move Up";
			case ERow::MoveDown:      return "Move Down";
			case ERow::MoveFast:      return "Move Fast";
			case ERow::OpenWorldMenu: return "World Settings";
			case ERow::MouseSensitivity: return "Mouse Sensitivity";
			case ERow::InvertMouse:   return "Invert Mouse";
			case ERow::ThrowBall:     return "Throw Ball";
			case ERow::OpenMenu:      return "Menu";
			case ERow::WindSpeed:     return "Wind Speed";
			case ERow::WindDirection: return "Wind Direction";
			case ERow::TimeSpeed:     return "Time Speed";
			case ERow::TimeOfDay:     return "Time of Day";
			case ERow::DayNumber:     return "Day";
			case ERow::WaveHeight:    return "Wave Height";
			case ERow::CloudCover:    return "Cloud Cover";
			case ERow::ResetWorld:    return "Reset to Defaults";
			default:                  return "Back";
			}
		}

		int Cycle(int Value, int Count, int Step)
		{
			return (Value + Step + Count) % Count;
		}

		const std::vector<ERow>& GetRows(Menu::EPage Page)
		{
			static const std::vector<ERow> MainRows = { ERow::Settings, ERow::Exit };
			static const std::vector<ERow> GraphicsRows = {
				ERow::Tabs, ERow::Preset, ERow::Backend, ERow::WindowMode, ERow::RenderScale, ERow::Upscaling, ERow::LowLatency, ERow::RayTracing, ERow::Shadows,
				ERow::GeometryDetail, ERow::ViewDistance, ERow::EyeFocus, ERow::AntiAliasing, ERow::Clouds, ERow::Back };
			static const std::vector<ERow> ControlRows = {
				ERow::Tabs, ERow::MoveForward, ERow::MoveBack, ERow::MoveLeft, ERow::MoveRight, ERow::MoveUp, ERow::MoveDown, ERow::MoveFast, ERow::OpenWorldMenu,
				ERow::MouseSensitivity, ERow::InvertMouse, ERow::ThrowBall, ERow::OpenMenu, ERow::Back };
			static const std::vector<ERow> WorldRows = {
				ERow::WindSpeed, ERow::WindDirection, ERow::TimeSpeed, ERow::TimeOfDay, ERow::DayNumber, ERow::WaveHeight, ERow::CloudCover, ERow::ResetWorld };
			switch (Page)
			{
			case Menu::EPage::Main:     return MainRows;
			case Menu::EPage::Controls: return ControlRows;
			case Menu::EPage::World:    return WorldRows;
			default:                    return GraphicsRows;
			}
		}

		struct Layout
		{
			float Scale;
			float RowHeight;
			float PanelX;
			float PanelY;
			float PanelWidth;
			float PanelHeight;
			float RowsY;
		};

		Layout MakeLayout(Menu::EPage Page, float WindowWidth, float WindowHeight)
		{
			const bool bMainPage = Page == Menu::EPage::Main;
			const bool bWorldPage = Page == Menu::EPage::World;
			const float Rows = static_cast<float>(GetRows(Page).size());

			Layout Result;
			Result.Scale = std::clamp(WindowHeight / 1080.0f, 0.6f, 2.0f);
			Result.RowHeight = 52.0f * Result.Scale;
			Result.PanelWidth = (bMainPage ? 420.0f : (bWorldPage ? 520.0f : 680.0f)) * Result.Scale;

			const float TitleHeight = 96.0f * Result.Scale;
			const float Footer = (bMainPage || bWorldPage ? 24.0f : 64.0f) * Result.Scale;
			Result.PanelHeight = TitleHeight + Result.RowHeight * Rows + Footer;

			// The world menu keeps to the side, out of the way of the world it changes.
			Result.PanelX = bWorldPage ? WindowWidth - Result.PanelWidth - 24.0f * Result.Scale : (WindowWidth - Result.PanelWidth) * 0.5f;
			Result.PanelY = (WindowHeight - Result.PanelHeight) * 0.5f;
			Result.RowsY = Result.PanelY + TitleHeight;
			return Result;
		}

		/** Moves a setting to its next value. Returns false for rows that hold no value. */
		bool ChangeValue(ERow Row, GameSettings& Settings, const MenuContext& Context, WorldClock& Clock, int Step)
		{
			const bool bUpscaling = Context.bHasUpscaling && Settings.Render.Upscaling != 0; // takes over resolution and anti-aliasing
			const bool bTracing = Context.bHasRayTracing && Settings.Render.RayTracing != 0; // takes over shadows
			switch (Row)
			{
			case ERow::Preset:
			{
				// From a custom mix, stepping starts at the ends of the list.
				const int Preset = Settings.GetPreset();
				const int Start = Preset >= 0 ? Preset : (Step > 0 ? -1 : 0);
				Settings.ApplyPreset(Cycle(Start, GameSettings::PresetCount, Step));
				return true;
			}
			case ERow::GeometryDetail:
				Settings.Render.GeometryDetail = Cycle(Settings.Render.GeometryDetail, 4, Step);
				return true;
			case ERow::Upscaling:
				if (Context.bHasUpscaling)
				{
					Settings.Render.Upscaling = Cycle(Settings.Render.Upscaling, 6, Step);
				}
				return Context.bHasUpscaling;
			case ERow::LowLatency:
				if (Context.bHasLowLatency)
				{
					Settings.Render.LowLatency = Cycle(Settings.Render.LowLatency, 3, Step);
				}
				return Context.bHasLowLatency;
			case ERow::RayTracing:
				if (Context.bHasRayTracing)
				{
					Settings.Render.RayTracing = Cycle(Settings.Render.RayTracing, 3, Step);
				}
				return Context.bHasRayTracing;
			case ERow::Backend:
				Settings.Backend = static_cast<Rhi::EBackend>(Cycle(static_cast<int>(Settings.Backend), 3, Step));
				return true;
			case ERow::WindowMode:
				Settings.bBorderless = !Settings.bBorderless;
				return true;
			case ERow::RenderScale:
				if (!bUpscaling)
				{
					Settings.Render.RenderScale = 50 + Cycle((Settings.Render.RenderScale - 50) / 25, 3, Step) * 25;
				}
				return !bUpscaling;
			case ERow::Shadows:
				if (!bTracing)
				{
					Settings.Render.ShadowQuality = Cycle(Settings.Render.ShadowQuality, 4, Step);
				}
				return !bTracing;
			case ERow::ViewDistance:
				Settings.ViewDistance = Cycle(Settings.ViewDistance, GameSettings::ViewDistanceCount, Step);
				return true;
			case ERow::EyeFocus:
				Settings.Render.bEyeFocus = !Settings.Render.bEyeFocus;
				return true;
			case ERow::AntiAliasing:
				if (!bUpscaling)
				{
					Settings.Render.bFxaa = !Settings.Render.bFxaa;
				}
				return !bUpscaling;
			case ERow::Clouds:
				Settings.Render.bClouds = !Settings.Render.bClouds;
				return true;
			case ERow::MouseSensitivity:
				Settings.Controls.MouseSensitivity = std::clamp(Settings.Controls.MouseSensitivity + Step * ControlSettings::SensitivityStep,
					ControlSettings::MinSensitivity, ControlSettings::MaxSensitivity);
				return true;
			case ERow::InvertMouse:
				Settings.Controls.bInvertMouse = !Settings.Controls.bInvertMouse;
				return true;
			case ERow::WindSpeed:
				Settings.World.WindSpeed = std::clamp(Settings.World.WindSpeed + Step * WorldSettings::WindStep, 0.0, WorldSettings::MaxWindSpeed);
				return true;
			case ERow::WindDirection:
				// One step past the ends of the compass is the wind that changes by itself.
				Settings.World.WindDirection = Cycle(Settings.World.WindDirection + 1, WorldSettings::WindDirections + 1, Step) - 1;
				return true;
			case ERow::TimeSpeed:
				Settings.World.TimeSpeed = std::clamp(Settings.World.TimeSpeed + Step, 0, WorldSettings::TimeRates - 1);
				return true;
			case ERow::TimeOfDay:
				Clock.Hour = std::fmod(std::floor(Clock.Hour * 2.0 + 0.001) / 2.0 + Step * 0.5 + 24.0, 24.0);
				return true;
			case ERow::DayNumber:
				Clock.Day = std::max(Clock.Day + Step, 1.0);
				return true;
			case ERow::WaveHeight:
				Settings.World.WaveHeight = std::clamp(Settings.World.WaveHeight + Step * WorldSettings::WaveStep, 0.0, WorldSettings::MaxWaveHeight);
				return true;
			case ERow::CloudCover:
				Settings.World.CloudCover = std::clamp(Settings.World.CloudCover + Step * WorldSettings::CloudStep, 0.0, WorldSettings::MaxCloudCover);
				return true;
			case ERow::ResetWorld:
				Settings.World = WorldSettings{};
				return true;
			default:
				return false;
			}
		}
	}

	void Menu::SetPage(EPage NewPage)
	{
		Page = NewPage;
		bWaitingForKey = false;
		Selected = NewPage == EPage::Graphics || NewPage == EPage::Controls ? 1 : 0; // the first row after the tabs
	}

	bool Menu::HandleInput(const Window& Input, GameSettings& Settings, const MenuContext& Context, WorldClock& Clock)
	{
		const std::vector<ERow>& Rows = GetRows(Page);
		const int RowCount = static_cast<int>(Rows.size());
		const ERow Waiting = Rows[Selected];

		// While a row waits for its key, every key goes to it. Esc gives up, as does a click.
		if (bWaitingForKey)
		{
			EKey Key = EKey::Escape;
			if (Input.GetPressedKey(Key))
			{
				bWaitingForKey = false;
				if (Key != EKey::Escape)
				{
					Settings.Controls.Bind(GetAction(Waiting), Key);
					return true;
				}
			}
			else if (Input.WasMousePressed(EMouseButton::Left))
			{
				bWaitingForKey = false;
			}
			return false;
		}

		const float Width = static_cast<float>(Input.GetWidth());
		const float Height = static_cast<float>(Input.GetHeight());
		const Layout Hit = MakeLayout(Page, Width, Height);

		// Hover only takes over when the mouse itself moves, so it does not fight the keyboard.
		// Cursor coordinates cannot tell: they also change when the window moves under them.
		const int MouseX = Input.GetMouseX();
		const int MouseY = Input.GetMouseY();
		const bool bMouseMoved = Input.GetMouseDeltaX() != 0.0 || Input.GetMouseDeltaY() != 0.0;

		const int HoveredRow = static_cast<float>(MouseX) >= Hit.PanelX && static_cast<float>(MouseX) < Hit.PanelX + Hit.PanelWidth
			&& static_cast<float>(MouseY) >= Hit.RowsY
			? static_cast<int>((static_cast<float>(MouseY) - Hit.RowsY) / Hit.RowHeight)
			: -1;
		const bool bOverRow = Context.bUseMouse && HoveredRow >= 0 && HoveredRow < RowCount;
		if (bOverRow && bMouseMoved)
		{
			Selected = HoveredRow;
		}

		if (Input.WasKeyPressed(EKey::Up))
		{
			Selected = Cycle(Selected, RowCount, -1);
		}
		if (Input.WasKeyPressed(EKey::Down))
		{
			Selected = Cycle(Selected, RowCount, 1);
		}

		const bool bActivate = Input.WasKeyPressed(EKey::Enter) || Input.WasKeyPressed(EKey::Space)
			|| (bOverRow && Input.WasMousePressed(EMouseButton::Left));

		bool bChanged = false;
		const ERow Current = Rows[Selected];

		if (Current == ERow::Tabs)
		{
			const EPage Other = Page == EPage::Graphics ? EPage::Controls : EPage::Graphics;
			const bool bClicked = bOverRow && Input.WasMousePressed(EMouseButton::Left);
			const bool bOnOther = !bClicked || (static_cast<float>(MouseX) < Hit.PanelX + Hit.PanelWidth * 0.5f) == (Other == EPage::Graphics);
			if (Input.WasKeyPressed(EKey::Left) || Input.WasKeyPressed(EKey::Right) || (bActivate && bOnOther))
			{
				SetPage(Other);
				Selected = 0;
			}
		}
		else
		{
			if (Input.WasKeyPressed(EKey::Left))
			{
				bChanged = ChangeValue(Current, Settings, Context, Clock, -1);
			}
			if (Input.WasKeyPressed(EKey::Right))
			{
				bChanged = ChangeValue(Current, Settings, Context, Clock, 1);
			}

			if (bActivate)
			{
				switch (Current)
				{
				case ERow::Settings:
					SetPage(EPage::Graphics);
					break;
				case ERow::Exit:
					bExitRequested = true;
					break;
				case ERow::Back:
					SetPage(EPage::Main);
					break;
				default:
					if (IsKeyRow(Current))
					{
						bWaitingForKey = true;
					}
					else
					{
						bChanged = ChangeValue(Current, Settings, Context, Clock, 1);
					}
					break;
				}
			}
		}

		const bool bWorldKey = Input.WasKeyPressed(Settings.Controls.Get(EAction::WorldMenu));
		if (Page == EPage::World && (Input.WasKeyPressed(EKey::Escape) || bWorldKey))
		{
			SetPage(EPage::Closed);
		}
		else if (Input.WasKeyPressed(EKey::Escape))
		{
			SetPage(Page == EPage::Main ? EPage::Closed : EPage::Main);
		}
		return bChanged;
	}

	bool Menu::Update(const Window& Input, Renderer& Output, GameSettings& Settings, const MenuContext& Context, WorldClock& Clock)
	{
		if (Page == EPage::Closed)
		{
			if (Input.WasKeyPressed(EKey::Escape))
			{
				SetPage(EPage::Main);
			}
			else if (Input.WasKeyPressed(Settings.Controls.Get(EAction::WorldMenu)))
			{
				SetPage(EPage::World);
			}
			return false;
		}

		const bool bChanged = HandleInput(Input, Settings, Context, Clock);
		if (Page == EPage::Closed)
		{
			return bChanged;
		}

		// Drawn from the page as it stands now, which input above may have just switched.
		const float Width = static_cast<float>(Input.GetWidth());
		const float Height = static_cast<float>(Input.GetHeight());
		const Layout Area = MakeLayout(Page, Width, Height);
		if (Page != EPage::World)
		{
			Output.DrawRect(0.0f, 0.0f, Width, Height, Dim);
		}
		Output.DrawRect(Area.PanelX, Area.PanelY, Area.PanelWidth, Area.PanelHeight, Panel);

		const char* Title = Page == EPage::Main ? "Paused" : (Page == EPage::World ? "World" : "Settings");
		const float TitleSize = 38.0f * Area.Scale;
		Output.DrawText(Area.PanelX + (Area.PanelWidth - Output.MeasureText(TitleSize, Title)) * 0.5f, Area.PanelY + 24.0f * Area.Scale, TitleSize, Title, TextColor);

		const std::vector<ERow>& DrawnRows = GetRows(Page);
		const float RowSize = 26.0f * Area.Scale;
		const float Inset = 32.0f * Area.Scale;
		for (int Index = 0; Index < static_cast<int>(DrawnRows.size()); ++Index)
		{
			const float Y = Area.RowsY + Area.RowHeight * static_cast<float>(Index);
			if (Index == Selected)
			{
				Output.DrawRect(Area.PanelX + 12.0f * Area.Scale, Y + 3.0f * Area.Scale, Area.PanelWidth - 24.0f * Area.Scale, Area.RowHeight - 6.0f * Area.Scale, Highlight);
			}

			const ERow Row = DrawnRows[static_cast<std::size_t>(Index)];
			const float TextY = Y + (Area.RowHeight - RowSize * 1.33f) * 0.5f;
			if (Row == ERow::Tabs)
			{
				// One half of the row for each tab, with a line under the one that is open.
				const char* const Names[] = { "Graphics", "Controls" };
				for (int Tab = 0; Tab < 2; ++Tab)
				{
					const bool bOpen = (Tab == 1) == (Page == EPage::Controls);
					const float Half = Area.PanelWidth * 0.5f;
					const float Left = Area.PanelX + Half * static_cast<float>(Tab);
					const float TextWidth = Output.MeasureText(RowSize, Names[Tab]);
					Output.DrawText(Left + (Half - TextWidth) * 0.5f, TextY, RowSize, Names[Tab], bOpen ? TextColor : ValueColor);
					if (bOpen)
					{
						Output.DrawRect(Left + (Half - TextWidth) * 0.5f, Y + Area.RowHeight - 7.0f * Area.Scale, TextWidth, 2.0f * Area.Scale, ValueColor);
					}
				}
				continue;
			}

			const bool bAsking = bWaitingForKey && Index == Selected;
			const std::string Value = bAsking ? "Press a key, Esc to cancel" : GetValue(Row, Settings, Context, Clock);
			if (Value.empty())
			{
				const char* Label = GetLabel(Row);
				Output.DrawText(Area.PanelX + (Area.PanelWidth - Output.MeasureText(RowSize, Label)) * 0.5f, TextY, RowSize, Label, TextColor);
			}
			else
			{
				Output.DrawText(Area.PanelX + Inset, TextY, RowSize, GetLabel(Row), TextColor);
				Output.DrawText(Area.PanelX + Area.PanelWidth - Inset - Output.MeasureText(RowSize, Value.c_str()), TextY, RowSize, Value.c_str(), ValueColor);
			}
		}

		if (Page == EPage::Graphics && Settings.Backend != Rhi::EBackend::Auto && Settings.Backend != Context.RunningBackend)
		{
			const char* Hint = "Restart the game to apply the graphics API";
			const float HintSize = 20.0f * Area.Scale;
			Output.DrawText(Area.PanelX + (Area.PanelWidth - Output.MeasureText(HintSize, Hint)) * 0.5f, Area.PanelY + Area.PanelHeight - 44.0f * Area.Scale, HintSize, Hint, HintColor);
		}
		return bChanged;
	}
}
