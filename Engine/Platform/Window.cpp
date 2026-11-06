#include "Platform/Window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>

#include <dbghelp.h>
#include <windowsx.h>

#include "Core/Log.h"

#include <algorithm>
#include <cstring>

namespace Teinora
{
	namespace
	{
		const wchar_t* const ClassName = L"TeinoraWindow";

		LONG WINAPI OnCrash(EXCEPTION_POINTERS* Info)
		{
			Log("Crash: exception 0x%08X at %p", static_cast<unsigned>(Info->ExceptionRecord->ExceptionCode), Info->ExceptionRecord->ExceptionAddress);

			const HANDLE Process = GetCurrentProcess();
			SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
			SymInitialize(Process, nullptr, TRUE);

			CONTEXT Context = *Info->ContextRecord;
			STACKFRAME64 Frame = {};
			Frame.AddrPC.Offset = Context.Rip;
			Frame.AddrPC.Mode = AddrModeFlat;
			Frame.AddrStack.Offset = Context.Rsp;
			Frame.AddrStack.Mode = AddrModeFlat;
			Frame.AddrFrame.Offset = Context.Rbp;
			Frame.AddrFrame.Mode = AddrModeFlat;

			alignas(SYMBOL_INFO) char Buffer[sizeof(SYMBOL_INFO) + 256];
			SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(Buffer);
			for (int Depth = 0; Depth < 32; ++Depth)
			{
				if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, Process, GetCurrentThread(), &Frame, &Context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
				{
					break;
				}

				std::memset(Buffer, 0, sizeof(Buffer));
				Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				Symbol->MaxNameLen = 255;
				DWORD64 Offset = 0;
				const bool bNamed = SymFromAddr(Process, Frame.AddrPC.Offset, &Offset, Symbol) != FALSE;

				IMAGEHLP_LINE64 Line = {};
				Line.SizeOfStruct = sizeof(Line);
				DWORD LineOffset = 0;
				if (SymGetLineFromAddr64(Process, Frame.AddrPC.Offset, &LineOffset, &Line))
				{
					Log("  %s (%s:%lu)", bNamed ? Symbol->Name : "?", Line.FileName, Line.LineNumber);
				}
				else
				{
					Log("  %s (%p)", bNamed ? Symbol->Name : "?", reinterpret_cast<void*>(Frame.AddrPC.Offset));
				}
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	void InstallCrashHandler()
	{
		SetUnhandledExceptionFilter(&OnCrash);
	}

	FontAtlas RasterizeFont(int PixelHeight)
	{
		const HDC Context = CreateCompatibleDC(nullptr);
		const HFONT Font = CreateFontW(-PixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
		SelectObject(Context, Font);

		TEXTMETRICW Metrics = {};
		GetTextMetricsW(Context, &Metrics);

		FontAtlas Atlas;
		Atlas.CellWidth = PixelHeight + 2;
		Atlas.CellHeight = Metrics.tmHeight + 2;
		Atlas.Width = Atlas.CellWidth * FontAtlas::Columns;
		Atlas.Height = Atlas.CellHeight * FontAtlas::Rows;

		BITMAPINFO Info = {};
		Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		Info.bmiHeader.biWidth = Atlas.Width;
		Info.bmiHeader.biHeight = -Atlas.Height; // top-down
		Info.bmiHeader.biPlanes = 1;
		Info.bmiHeader.biBitCount = 32;
		Info.bmiHeader.biCompression = BI_RGB;

		void* Bits = nullptr;
		const HBITMAP Bitmap = CreateDIBSection(Context, &Info, DIB_RGB_COLORS, &Bits, nullptr, 0);
		SelectObject(Context, Bitmap);
		SetBkColor(Context, RGB(0, 0, 0));
		SetTextColor(Context, RGB(255, 255, 255));
		SetBkMode(Context, TRANSPARENT);

		for (int Glyph = 0; Glyph < FontAtlas::GlyphCount; ++Glyph)
		{
			const wchar_t Character = static_cast<wchar_t>(FontAtlas::FirstGlyph + Glyph);
			const int CellX = (Glyph % FontAtlas::Columns) * Atlas.CellWidth;
			const int CellY = (Glyph / FontAtlas::Columns) * Atlas.CellHeight;
			TextOutW(Context, CellX + 1, CellY + 1, &Character, 1);

			INT Width = 0;
			GetCharWidth32W(Context, Character, Character, &Width);
			Atlas.Advance[Glyph] = static_cast<float>(Width);
		}
		GdiFlush();

		Atlas.Pixels.resize(static_cast<std::size_t>(Atlas.Width) * Atlas.Height * 4);
		const std::uint8_t* Source = static_cast<const std::uint8_t*>(Bits);
		for (std::size_t Index = 0; Index < static_cast<std::size_t>(Atlas.Width) * Atlas.Height; ++Index)
		{
			Atlas.Pixels[Index * 4 + 0] = 255;
			Atlas.Pixels[Index * 4 + 1] = 255;
			Atlas.Pixels[Index * 4 + 2] = 255;
			Atlas.Pixels[Index * 4 + 3] = Source[Index * 4];
		}

		const int WhiteX = (FontAtlas::WhiteCell % FontAtlas::Columns) * Atlas.CellWidth;
		const int WhiteY = (FontAtlas::WhiteCell / FontAtlas::Columns) * Atlas.CellHeight;
		for (int Y = 0; Y < Atlas.CellHeight; ++Y)
		{
			for (int X = 0; X < Atlas.CellWidth; ++X)
			{
				Atlas.Pixels[(static_cast<std::size_t>(WhiteY + Y) * Atlas.Width + WhiteX + X) * 4 + 3] = 255;
			}
		}

		DeleteObject(Bitmap);
		DeleteObject(Font);
		DeleteDC(Context);
		return Atlas;
	}

	void ShowErrorMessage(const char* Text)
	{
		MessageBoxA(nullptr, Text, "Winds of Teinora", MB_OK | MB_ICONERROR);
	}

	std::string GetExecutableDirectory()
	{
		char Path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, Path, MAX_PATH);
		std::string Result = Path;
		return Result.substr(0, Result.find_last_of("\\/") + 1);
	}

	Window::Window(const wchar_t* Title, int Width, int Height, bool bActivate) : bTakesFocus(bActivate)
	{
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

		const HINSTANCE Instance = GetModuleHandleW(nullptr);

		WNDCLASSEXW Class = {};
		Class.cbSize = sizeof(Class);
		Class.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::HandleMessage);
		Class.hInstance = Instance;
		Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		Class.lpszClassName = ClassName;
		RegisterClassExW(&Class);

		RECT Frame = { 0, 0, Width, Height };
		AdjustWindowRect(&Frame, WS_OVERLAPPEDWINDOW, FALSE);

		const HWND Created = CreateWindowExW(0, ClassName, Title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
			Frame.right - Frame.left, Frame.bottom - Frame.top, nullptr, nullptr, Instance, this);
		Handle = Created;

		RECT Client;
		GetClientRect(Created, &Client);
		ClientWidth = Client.right;
		ClientHeight = Client.bottom;

		// Raw input gives mouse motion without acceleration, and it keeps working at the screen edge.
		RAWINPUTDEVICE Mouse = {};
		Mouse.usUsagePage = 0x01;
		Mouse.usUsage = 0x02;
		Mouse.hwndTarget = Created;
		RegisterRawInputDevices(&Mouse, 1, sizeof(Mouse));

		// The first ShowWindow of a process obeys how it was launched, which may be hidden.
		const int Show = bActivate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE;
		ShowWindow(Created, Show);
		ShowWindow(Created, Show);
	}

	Window::~Window()
	{
		ClipCursor(nullptr);
		DestroyWindow(static_cast<HWND>(Handle));
		UnregisterClassW(ClassName, GetModuleHandleW(nullptr));
	}

	void* Window::GetNativeHandle() const
	{
		return Handle;
	}

	void Window::Close()
	{
		bOpen = false;
	}

	bool Window::PollEvents()
	{
		std::memset(KeysPressed, 0, sizeof(KeysPressed));
		std::memset(ButtonsPressed, 0, sizeof(ButtonsPressed));

		MSG Message;
		while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}

		MouseDeltaX = PendingDeltaX;
		MouseDeltaY = PendingDeltaY;
		PendingDeltaX = 0.0;
		PendingDeltaY = 0.0;
		return bOpen;
	}

	bool Window::GetPressedKey(EKey& OutKey) const
	{
		for (int Key = 1; Key < 256; ++Key)
		{
			if (KeysPressed[Key])
			{
				OutKey = static_cast<EKey>(Key);
				return true;
			}
		}
		return false;
	}

	std::string GetKeyName(EKey Key)
	{
		const int Code = static_cast<int>(Key);
		if ((Code >= '0' && Code <= '9') || (Code >= 'A' && Code <= 'Z'))
		{
			return std::string(1, static_cast<char>(Code));
		}
		if (Code >= VK_F1 && Code <= VK_F24)
		{
			return "F" + std::to_string(Code - VK_F1 + 1);
		}
		switch (Code)
		{
		case VK_SPACE:   return "Space";
		case VK_SHIFT:   return "Shift";
		case VK_CONTROL: return "Ctrl";
		case VK_MENU:    return "Alt";
		case VK_TAB:     return "Tab";
		case VK_RETURN:  return "Enter";
		case VK_BACK:    return "Backspace";
		case VK_CAPITAL: return "Caps Lock";
		case VK_LEFT:    return "Left Arrow";
		case VK_RIGHT:   return "Right Arrow";
		case VK_UP:      return "Up Arrow";
		case VK_DOWN:    return "Down Arrow";
		case VK_INSERT:  return "Insert";
		case VK_DELETE:  return "Delete";
		case VK_HOME:    return "Home";
		case VK_END:     return "End";
		case VK_PRIOR:   return "Page Up";
		case VK_NEXT:    return "Page Down";
		case VK_OEM_2:   return "/";
		default:         break;
		}
		if (Code >= VK_NUMPAD0 && Code <= VK_NUMPAD9)
		{
			return "Num " + std::to_string(Code - VK_NUMPAD0);
		}

		// Punctuation depends on the keyboard. The on-screen font only has plain ASCII.
		const UINT Character = MapVirtualKeyW(static_cast<UINT>(Code), MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
		if (Character > 32 && Character < 127)
		{
			return std::string(1, static_cast<char>(Character));
		}
		return "Key " + std::to_string(Code);
	}

	void Window::SetMouseCaptured(bool bCaptured)
	{
		if (bMouseCaptured != bCaptured)
		{
			bMouseCaptured = bCaptured;
			ApplyCapture();
		}
	}

	void Window::ApplyCapture()
	{
		if (bMouseCaptured && bFocused)
		{
			RECT Client;
			GetClientRect(static_cast<HWND>(Handle), &Client);
			MapWindowPoints(static_cast<HWND>(Handle), nullptr, reinterpret_cast<POINT*>(&Client), 2);
			ClipCursor(&Client);
		}
		else
		{
			ClipCursor(nullptr);
		}
		SetCursor(bMouseCaptured && bFocused ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
	}

	void Window::SetBorderless(bool bBorderless)
	{
		if (bIsBorderless == bBorderless)
		{
			return;
		}
		bIsBorderless = bBorderless;

		const HWND Target = static_cast<HWND>(Handle);
		const UINT Flags = SWP_FRAMECHANGED | (bTakesFocus ? 0 : SWP_NOACTIVATE);
		if (bBorderless)
		{
			RECT Current;
			GetWindowRect(Target, &Current);
			SavedRect[0] = Current.left;
			SavedRect[1] = Current.top;
			SavedRect[2] = Current.right - Current.left;
			SavedRect[3] = Current.bottom - Current.top;
			SavedStyle = GetWindowLongPtrW(Target, GWL_STYLE);

			MONITORINFO Monitor = { sizeof(Monitor) };
			GetMonitorInfoW(MonitorFromWindow(Target, MONITOR_DEFAULTTONEAREST), &Monitor);
			SetWindowLongPtrW(Target, GWL_STYLE, WS_POPUP | WS_VISIBLE);
			SetWindowPos(Target, HWND_TOP, Monitor.rcMonitor.left, Monitor.rcMonitor.top,
				Monitor.rcMonitor.right - Monitor.rcMonitor.left, Monitor.rcMonitor.bottom - Monitor.rcMonitor.top, Flags);
		}
		else
		{
			SetWindowLongPtrW(Target, GWL_STYLE, SavedStyle);
			SetWindowPos(Target, HWND_NOTOPMOST, SavedRect[0], SavedRect[1], SavedRect[2], SavedRect[3], Flags);
		}
		ApplyCapture();
	}

	long long __stdcall Window::HandleMessage(void* Handle, unsigned Message, unsigned long long WParam, long long LParam)
	{
		const HWND Target = static_cast<HWND>(Handle);
		if (Message == WM_NCCREATE)
		{
			const CREATESTRUCTW* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
			SetWindowLongPtrW(Target, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Create->lpCreateParams));
		}

		Window* Self = reinterpret_cast<Window*>(GetWindowLongPtrW(Target, GWLP_USERDATA));
		if (Self != nullptr)
		{
			Self->Handle = Target;
			return Self->ProcessMessage(Message, WParam, LParam);
		}
		return DefWindowProcW(Target, Message, WParam, LParam);
	}

	long long Window::ProcessMessage(unsigned Message, unsigned long long WParam, long long LParam)
	{
		const HWND Target = static_cast<HWND>(Handle);

		switch (Message)
		{
		case WM_CLOSE:
			bOpen = false;
			return 0;

		case WM_SIZE:
			ClientWidth = LOWORD(LParam);
			ClientHeight = HIWORD(LParam);
			ApplyCapture();
			return 0;

		case WM_SETFOCUS:
			bFocused = true;
			ApplyCapture();
			return 0;

		case WM_KILLFOCUS:
			bFocused = false;
			std::memset(Keys, 0, sizeof(Keys));
			std::memset(Buttons, 0, sizeof(Buttons));
			ApplyCapture();
			return 0;

		case WM_SETCURSOR:
			if (LOWORD(LParam) == HTCLIENT)
			{
				SetCursor(bMouseCaptured && bFocused ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
				return TRUE;
			}
			break;

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (WParam < 256)
			{
				KeysPressed[WParam] = KeysPressed[WParam] || !Keys[WParam];
				Keys[WParam] = true;
			}
			if (WParam == VK_MENU || WParam == VK_F10)
			{
				return 0; // keep the system menu from stealing input
			}
			break;

		case WM_KEYUP:
		case WM_SYSKEYUP:
			if (WParam < 256)
			{
				Keys[WParam] = false;
			}
			break;

		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		{
			const int Button = Message == WM_LBUTTONDOWN ? 0 : 1;
			Buttons[Button] = true;
			ButtonsPressed[Button] = true;
			SetCapture(Target);
			return 0;
		}

		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
			Buttons[Message == WM_LBUTTONUP ? 0 : 1] = false;
			if (!Buttons[0] && !Buttons[1])
			{
				ReleaseCapture();
			}
			return 0;

		case WM_MOUSEMOVE:
			MouseX = GET_X_LPARAM(LParam);
			MouseY = GET_Y_LPARAM(LParam);
			return 0;

		case WM_INPUT:
		{
			RAWINPUT Raw;
			UINT Size = sizeof(Raw);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(LParam), RID_INPUT, &Raw, &Size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
				&& Raw.header.dwType == RIM_TYPEMOUSE && (Raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
			{
				PendingDeltaX += Raw.data.mouse.lLastX;
				PendingDeltaY += Raw.data.mouse.lLastY;
			}
			break;
		}

		default:
			break;
		}

		return DefWindowProcW(Target, Message, WParam, LParam);
	}
}
