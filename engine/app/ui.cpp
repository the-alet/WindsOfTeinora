#include "app/ui.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/world.h"

namespace taelia {

namespace {

void applyStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 8;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.TabRounding = 5;
    s.WindowBorderSize = 0;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowPadding = ImVec2(18, 16);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.88f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.20f, 0.9f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.27f, 0.30f, 0.9f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.38f, 0.42f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.17f, 0.19f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.33f, 0.36f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.62f, 0.66f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.62f, 0.80f, 0.82f, 1.0f);
    s.ScaleAllSizes(scale);
}

const char* compass(double fromDeg) {
    static const char* names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = int(std::floor(std::fmod(fromDeg + 22.5 + 360.0, 360.0) / 45.0)) % 8;
    return names[i];
}

void controlsTable() {
    static const char* rows[][2] = {
        {"Mouse", "Move the eye's focus; the view follows it"},
        {"W / S", "Fly forward / back"},
        {"A / D", "Fly left / right"},
        {"E or Space", "Up"},
        {"Q or C", "Down"},
        {"+ / -", "Time speed (x1/50 .. x50)"},
        {"M", "Magic view (arrives with the magic)"},
        {"/", "World settings"},
        {"Esc", "Menu"},
    };
    if (ImGui::BeginTable("controls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableSetupColumn("Action");
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
}

}  // namespace

bool Ui::init(void* hwnd, float dpiScale) {
    hwnd_ = hwnd;
    scale_ = dpiScale > 0 ? dpiScale : 1.0f;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font, 19.0f * scale_);
    else io.Fonts->AddFontDefault();
    applyStyle(scale_);
    if (hwnd_) ImGui_ImplWin32_Init(hwnd_);
    return true;
}

void Ui::shutdown() {
    if (!ImGui::GetCurrentContext()) return;
    if (hwnd_) ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void Ui::newFrame(uint32_t width, uint32_t height, double dt, bool mouseCaptured) {
    ImGuiIO& io = ImGui::GetIO();
    if (mouseCaptured) io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    if (hwnd_) ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = float(dt > 1e-6 ? dt : 1.0 / 60.0);
    ImGui::NewFrame();
}

UiActions Ui::build(World& world, GraphicsSettings& settings, const std::string& activeApi, bool showOverlay, bool dlss, bool reflex) {
    UiActions act;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float em = ImGui::GetFontSize();

    if (showOverlay) {
        ImGui::SetNextWindowPos(ImVec2(em * 0.8f, em * 0.6f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        const CalendarTime t = world.clock().calendar();
        const double speed = world.clock().timeSpeed();
        char speedText[32];
        if (speed >= 1.0) std::snprintf(speedText, sizeof speedText, "x%g", speed);
        else std::snprintf(speedText, sizeof speedText, "x1/%g", std::round(1.0 / speed));
        ImGui::Text("Day %lld   %02d:%02d:%02d   %s", static_cast<long long>(t.day + 1), t.hour, t.minute, t.second, speedText);
        const WorldSettings& ws = world.settings();
        ImGui::Text("Wind %.1f m/s from %s", ws.windSpeed, compass(ws.windFromDeg));
        ImGui::End();
    }

    if (menu == Menu::Main) {
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(em * 30, 0));
        ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Graphics", nullptr, mainTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                const char* apis[] = {"Direct3D 12", "Vulkan"};
                int api = settings.api == "vulkan" ? 1 : 0;
                ImGui::SetNextItemWidth(em * 12);
                if (ImGui::Combo("Graphics API", &api, apis, 2)) {
                    settings.api = api == 1 ? "vulkan" : "d3d12";
                    act.settingsChanged = true;
                }
                if (settings.api != activeApi) ImGui::TextDisabled("Applies after a restart.");
                int detail = settings.detail;
                const char* details[] = {"Low", "Medium", "High", "Highest"};
                ImGui::SetNextItemWidth(em * 12);
                if (ImGui::Combo("Detail", &detail, details, 4)) {
                    settings.detail = detail;
                    act.settingsChanged = true;
                }
                if (dlss) {
                    const char* ups[] = {"Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
                    ImGui::SetNextItemWidth(em * 12);
                    if (ImGui::Combo("DLSS", &settings.upscaler, ups, 6)) act.settingsChanged = true;
                } else {
                    ImGui::TextDisabled("DLSS is not available on this device.");
                }
                if (!dlss || settings.upscaler == 0) {
                    int scalePct = int(std::lround(settings.renderScale * 100));
                    ImGui::SetNextItemWidth(em * 12);
                    if (ImGui::SliderInt("Render scale", &scalePct, 50, 100, "%d%%")) {
                        settings.renderScale = float(scalePct) / 100.0f;
                        act.settingsChanged = true;
                    }
                }
                if (reflex) {
                    const char* lat[] = {"Off", "On", "On + Boost"};
                    ImGui::SetNextItemWidth(em * 12);
                    if (ImGui::Combo("Low latency (Reflex)", &settings.latency, lat, 3)) act.settingsChanged = true;
                } else {
                    ImGui::TextDisabled("Reflex is not available on this device.");
                }
                if (ImGui::Checkbox("Fullscreen", &settings.fullscreen)) act.settingsChanged = true;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls", nullptr, mainTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                controlsTable();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        mainTab = -1;  // only force the tab on the first frame
        ImGui::Separator();
        if (ImGui::Button("Resume", ImVec2(em * 8, 0))) menu = Menu::None;
        ImGui::SameLine();
        if (ImGui::Button("Exit", ImVec2(em * 8, 0))) act.exit = true;
        ImGui::End();
    } else if (menu == Menu::World) {
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(em * 26, 0));
        ImGui::Begin("World", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        WorldSettings& ws = world.settings();
        float wind = float(ws.windSpeed), from = float(ws.windFromDeg);
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::SliderFloat("Wind speed", &wind, 0.0f, 20.0f, "%.1f m/s")) ws.windSpeed = wind;
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::SliderFloat("Wind from", &from, 0.0f, 359.0f, "%.0f deg")) ws.windFromDeg = from;
        float hour = float(world.clock().hourOfDay());
        ImGui::SetNextItemWidth(em * 12);
        if (ImGui::SliderFloat("Time of day", &hour, 0.0f, float(k::HOURS_PER_DAY) - 0.01f, "%.2f h")) world.clock().jumpToHour(hour);
        if (ImGui::Button(" - ")) world.clock().slower();
        ImGui::SameLine();
        if (ImGui::Button(" + ")) world.clock().faster();
        ImGui::SameLine();
        const double speed = world.clock().timeSpeed();
        if (speed >= 1.0) ImGui::Text("Time speed x%g", speed);
        else ImGui::Text("Time speed x1/%g", std::round(1.0 / speed));
        if (ImGui::Button("Previous day")) world.clock().jumpDays(-1);
        ImGui::SameLine();
        if (ImGui::Button("Next day")) world.clock().jumpDays(1);
        bool natural = ws.cloudCover < 0;
        if (ImGui::Checkbox("Natural weather", &natural)) ws.cloudCover = natural ? -1.0 : 0.5;
        if (!natural) {
            float cover = float(ws.cloudCover);
            ImGui::SetNextItemWidth(em * 12);
            if (ImGui::SliderFloat("Cloud cover", &cover, 0.0f, 1.0f, "%.2f")) ws.cloudCover = cover;
        }
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(em * 8, 0))) menu = Menu::None;
        ImGui::End();
    }
    return act;
}

ImDrawData* Ui::render() {
    ImGui::Render();
    return ImGui::GetDrawData();
}

}  // namespace taelia
