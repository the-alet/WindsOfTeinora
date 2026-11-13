#include "platform/window.h"

#include <windows.h>

#include "imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace taelia {

namespace {
const wchar_t* kClassName = L"WondersOfTaeliaWindow";
}

Window::~Window() { destroy(); }

void Window::enableDpiAwareness() { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }

bool Window::create(const std::wstring& title, int width, int height, bool maximized, bool show) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::wndProc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND h = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!h) return false;
    hwnd_ = h;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse; no RIDEV_INPUTSINK: only while we are in the foreground
    rid.hwndTarget = h;
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    if (show) ShowWindow(h, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);  // hidden windows never take focus
    UpdateWindow(h);
    RECT cr;
    GetClientRect(h, &cr);
    width_ = uint32_t(cr.right - cr.left);
    height_ = uint32_t(cr.bottom - cr.top);
    return true;
}

void Window::destroy() {
    if (!hwnd_) return;
    setMouseCaptured(false);
    DestroyWindow(static_cast<HWND>(hwnd_));
    hwnd_ = nullptr;
}

bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closed_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

bool Window::takeResized() {
    bool r = resized_;
    resized_ = false;
    return r;
}

bool Window::keyDown(int vk) const { return focused_ && vk >= 0 && vk < 256 && keys_[vk]; }
bool Window::keyPressed(int vk) const { return focused_ && vk >= 0 && vk < 256 && pressed_[vk]; }
bool Window::mouseDown(int b) const { return focused_ && b >= 0 && b < 3 && mouse_[b]; }
bool Window::mousePressed(int b) const { return focused_ && b >= 0 && b < 3 && mousePressed_[b]; }
bool Window::mouseReleased(int b) const { return b >= 0 && b < 3 && mouseReleased_[b]; }

void Window::beginInputFrame() {
    for (bool& p : pressed_) p = false;
    for (int i = 0; i < 3; ++i) mousePressed_[i] = mouseReleased_[i] = false;
}

void Window::takeMouseDelta(double& dx, double& dy) {
    dx = mouseDx_;
    dy = mouseDy_;
    mouseDx_ = mouseDy_ = 0;
}

void Window::clipCursor() {
    if (!captured_ || !focused_) {
        ClipCursor(nullptr);
        return;
    }
    RECT r;
    GetClientRect(static_cast<HWND>(hwnd_), &r);
    POINT a{r.left, r.top}, b{r.right, r.bottom};
    ClientToScreen(static_cast<HWND>(hwnd_), &a);
    ClientToScreen(static_cast<HWND>(hwnd_), &b);
    // Keep it at the centre so it never leaves the window while the raw deltas steer the gaze.
    const LONG cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    RECT c{cx, cy, cx + 1, cy + 1};
    ClipCursor(&c);
}

void Window::setMouseCaptured(bool captured) {
    if (captured == captured_) return;
    captured_ = captured;
    if (captured) {
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        while (ShowCursor(TRUE) < 0) {
        }
    }
    clipCursor();
    mouseDx_ = mouseDy_ = 0;
}

void Window::setBorderlessFullscreen(bool on) {
    HWND h = static_cast<HWND>(hwnd_);
    if (!h || on == fullscreen_) return;
    fullscreen_ = on;
    if (on) {
        RECT r;
        GetWindowRect(h, &r);
        savedRect_[0] = r.left, savedRect_[1] = r.top, savedRect_[2] = r.right, savedRect_[3] = r.bottom;
        savedStyle_ = GetWindowLongW(h, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(h, GWL_STYLE, savedStyle_ ? savedStyle_ : WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, nullptr, savedRect_[0], savedRect_[1], savedRect_[2] - savedRect_[0], savedRect_[3] - savedRect_[1],
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
    }
    clipCursor();
}

void Window::requestClose() { PostMessageW(static_cast<HWND>(hwnd_), WM_CLOSE, 0, 0); }

intptr_t __stdcall Window::wndProc(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    HWND h = static_cast<HWND>(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_) return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

intptr_t Window::handle(unsigned msg, uintptr_t wp, intptr_t lp) {
    HWND h = static_cast<HWND>(hwnd_);
    switch (msg) {
        case WM_CLOSE:
            closed_ = true;
            return 0;
        case WM_DESTROY:
            return 0;
        case WM_SIZE: {
            const uint32_t w = LOWORD(lp), hh = HIWORD(lp);
            if (w != width_ || hh != height_) resized_ = true;
            width_ = (wp == SIZE_MINIMIZED) ? 0 : w;
            height_ = (wp == SIZE_MINIMIZED) ? 0 : hh;
            clipCursor();
            return 0;
        }
        case WM_ACTIVATE:
            focused_ = LOWORD(wp) != WA_INACTIVE;
            if (!focused_) {
                for (bool& k : keys_) k = false;
                for (bool& m : mouse_) m = false;
            }
            clipCursor();
            return 0;
        case WM_INPUT: {
            if (!focused_) break;
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && captured_) {
                mouseDx_ += ri.data.mouse.lLastX;
                mouseDy_ += ri.data.mouse.lLastY;
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp < 256) {
                if (!keys_[wp]) pressed_[wp] = true;
                keys_[wp] = true;
            }
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // let Alt+F4 close
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) keys_[wp] = false;
            return 0;
        case WM_LBUTTONDOWN: mouse_[0] = mousePressed_[0] = true; SetCapture(h); return 0;
        case WM_LBUTTONUP: mouse_[0] = false; mouseReleased_[0] = true; ReleaseCapture(); return 0;
        case WM_RBUTTONDOWN: mouse_[1] = mousePressed_[1] = true; return 0;
        case WM_RBUTTONUP: mouse_[1] = false; mouseReleased_[1] = true; return 0;
        case WM_MBUTTONDOWN: mouse_[2] = mousePressed_[2] = true; return 0;
        case WM_MBUTTONUP: mouse_[2] = false; mouseReleased_[2] = true; return 0;
        case WM_MOUSEWHEEL: return 0;  // camera speed is fixed: the wheel does nothing
        case WM_SETCURSOR:
            if (captured_ && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace taelia
