// Win32 window and input. Automated runs never create one (they render headless), so they can't take focus.
#pragma once
#include <cstdint>
#include <string>

namespace taelia {

class Window {
public:
    ~Window();
    // Creates and shows the window (maximized unless a size is forced).
    bool create(const std::wstring& title, int width, int height, bool maximized, bool show = true);
    void destroy();
    // Handles pending messages. Returns false once the window was closed.
    bool pump();

    void* hwnd() const { return hwnd_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool minimized() const { return width_ == 0 || height_ == 0; }
    bool focused() const { return focused_; }
    // True once after the client size changed.
    bool takeResized();

    // Input (only while focused). Keys are Win32 virtual-key codes.
    bool keyDown(int vk) const;
    bool keyPressed(int vk) const;  // went down since the last beginInputFrame()
    bool mouseDown(int button) const;
    bool mousePressed(int button) const;
    bool mouseReleased(int button) const;
    void beginInputFrame();         // call once per frame before reading presses
    void takeMouseDelta(double& dx, double& dy);

    // Captured: cursor hidden and kept inside the window, raw deltas drive the gaze.
    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return captured_; }
    void setBorderlessFullscreen(bool on);
    bool borderlessFullscreen() const { return fullscreen_; }
    void requestClose();

    static void enableDpiAwareness();

private:
    static intptr_t __stdcall wndProc(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
    intptr_t handle(unsigned msg, uintptr_t wp, intptr_t lp);
    void clipCursor();

    void* hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool focused_ = false, resized_ = false, closed_ = false, captured_ = false, fullscreen_ = false;
    bool keys_[256] = {}, pressed_[256] = {};
    bool mouse_[3] = {}, mousePressed_[3] = {}, mouseReleased_[3] = {};
    double mouseDx_ = 0, mouseDy_ = 0;
    long savedRect_[4] = {};
    long savedStyle_ = 0;
};

}  // namespace taelia
