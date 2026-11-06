#pragma once

#include "Settings.h"

namespace Teinora
{
	class Renderer;
	class Window;

	/** What the running system offers, for the rows that depend on it. */
	struct MenuContext
	{
		Rhi::EBackend RunningBackend = Rhi::EBackend::Auto;
		bool bHasUpscaling = false;
		bool bHasLowLatency = false;
		bool bHasRayTracing = false;
		bool bUseMouse = true;      // an automated run leaves hover and clicks out of it
	};

	/** Where the world's day has got to, which the world menu can set. */
	struct WorldClock
	{
		double Hour = 10.0;
		double Day = 1.0;
	};

	/**
	 * The Esc menu: a main page with Settings and Exit, and settings with a tab each for graphics
	 * and controls. Beside it the world menu, on its own key, for wind, time and weather. The
	 * world goes on running behind that one, so that what it changes can be seen.
	 */
	class Menu
	{
	public:
		enum class EPage
		{
			Closed,
			Main,
			Graphics,
			Controls,
			World
		};

		EPage GetPage() const { return Page; }
		bool IsOpen() const { return Page != EPage::Closed; }
		bool PausesWorld() const { return Page != EPage::Closed && Page != EPage::World; }
		void SetPage(EPage NewPage);

		bool ShouldExit() const { return bExitRequested; }

		/** Handles input and queues drawing. Returns true when a setting has changed. */
		bool Update(const Window& Input, Renderer& Output, GameSettings& Settings, const MenuContext& Context, WorldClock& Clock);

	private:
		bool HandleInput(const Window& Input, GameSettings& Settings, const MenuContext& Context, WorldClock& Clock);

		EPage Page = EPage::Closed;
		int Selected = 0;
		bool bWaitingForKey = false; // the selected row takes the next key pressed
		bool bExitRequested = false;
	};
}
