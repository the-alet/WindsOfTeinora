// The operating system boundary: window, input, and a font rasterised by the OS.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Teinora
{
	// Values are Win32 virtual key codes.
	enum class EKey : int
	{
		Enter = 0x0D,
		Shift = 0x10,
		Control = 0x11,
		Escape = 0x1B,
		Space = 0x20,
		Left = 0x25,
		Up = 0x26,
		Right = 0x27,
		Down = 0x28,
		A = 'A',
		D = 'D',
		S = 'S',
		W = 'W',
		M = 'M',
		Slash = 0xBF // the key by the right shift, whatever the keyboard's layout puts on it
	};

	enum class EMouseButton : int
	{
		Left,
		Right,
		Count
	};

	struct FontAtlas
	{
		static constexpr int FirstGlyph = 32;
		static constexpr int GlyphCount = 95;
		static constexpr int Columns = 16;
		static constexpr int Rows = 6;
		static constexpr int WhiteCell = GlyphCount; // a solid cell, for drawing rectangles

		std::vector<std::uint8_t> Pixels; // RGBA8 with coverage in alpha
		int Width = 0;
		int Height = 0;
		int CellWidth = 0;
		int CellHeight = 0;
		float Advance[GlyphCount] = {};
	};

	/** Renders the printable ASCII range. PixelHeight is the em size of the glyphs. */
	FontAtlas RasterizeFont(int PixelHeight);

	void ShowErrorMessage(const char* Text);

	/** A short English name for any key, for showing what it is bound to. */
	std::string GetKeyName(EKey Key);

	/** Logs the exception and a symbolised call stack if the process crashes. */
	void InstallCrashHandler();

	/** The folder holding the running program, with a trailing separator. */
	std::string GetExecutableDirectory();

	class Window
	{
	public:
		/** With bActivate false the window opens without taking keyboard focus. */
		Window(const wchar_t* Title, int Width, int Height, bool bActivate);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		/** Pumps OS messages and refreshes per-frame input. Returns false once the window closes. */
		bool PollEvents();
		void Close();

		void* GetNativeHandle() const;
		int GetWidth() const { return ClientWidth; }
		int GetHeight() const { return ClientHeight; }
		bool HasFocus() const { return bFocused; }

		bool IsKeyDown(EKey Key) const { return Keys[static_cast<int>(Key)]; }
		bool WasKeyPressed(EKey Key) const { return KeysPressed[static_cast<int>(Key)]; }

		/** Whichever key went down this frame, for a key to be bound. False when none did. */
		bool GetPressedKey(EKey& OutKey) const;

		bool IsMouseDown(EMouseButton Button) const { return Buttons[static_cast<int>(Button)]; }
		bool WasMousePressed(EMouseButton Button) const { return ButtonsPressed[static_cast<int>(Button)]; }

		int GetMouseX() const { return MouseX; }
		int GetMouseY() const { return MouseY; }
		double GetMouseDeltaX() const { return MouseDeltaX; }
		double GetMouseDeltaY() const { return MouseDeltaY; }

		/** While captured, the cursor is hidden and held inside the window. */
		void SetMouseCaptured(bool bCaptured);

		/** Switches between a normal window and a borderless window covering the monitor. */
		void SetBorderless(bool bBorderless);

	private:
		static long long __stdcall HandleMessage(void* Handle, unsigned Message, unsigned long long WParam, long long LParam);
		long long ProcessMessage(unsigned Message, unsigned long long WParam, long long LParam);
		void ApplyCapture();

		void* Handle = nullptr;
		int ClientWidth = 0;
		int ClientHeight = 0;
		bool bTakesFocus = true;
		bool bOpen = true;
		bool bFocused = true;
		bool bMouseCaptured = false;
		bool bIsBorderless = false;
		long long SavedStyle = 0;
		int SavedRect[4] = {};

		bool Keys[256] = {};
		bool KeysPressed[256] = {};
		bool Buttons[static_cast<int>(EMouseButton::Count)] = {};
		bool ButtonsPressed[static_cast<int>(EMouseButton::Count)] = {};
		int MouseX = 0;
		int MouseY = 0;
		double MouseDeltaX = 0.0;
		double MouseDeltaY = 0.0;
		double PendingDeltaX = 0.0;
		double PendingDeltaY = 0.0;
	};
}
