// The UI: top-left lines (time, wind), the Esc menu (graphics, controls, exit), world settings on '/'.
#pragma once
#include <cstdint>
#include <string>

#include "app/settings.h"

struct ImDrawData;

namespace taelia {

class World;

enum class Menu { None, Main, World };

struct UiActions {
    bool exit = false;
    bool settingsChanged = false;  // save them; api changes apply on restart
};

class Ui {
public:
    // hwnd may be null (headless frames).
    bool init(void* hwnd, float dpiScale);
    void shutdown();
    void newFrame(uint32_t width, uint32_t height, double dt, bool mouseCaptured);
    UiActions build(World& world, GraphicsSettings& settings, const std::string& activeApi, bool showOverlay, bool dlss, bool reflex);
    ImDrawData* render();

    Menu menu = Menu::None;
    int mainTab = 0;  // 0 graphics, 1 controls
    bool playerMode = true;          // the camera rides the player (else the free camera of the checks)
    int ballAspect = 3;              // the magic ball's aspect (number keys)

private:
    void* hwnd_ = nullptr;
    float scale_ = 1.0f;
};

}  // namespace taelia
