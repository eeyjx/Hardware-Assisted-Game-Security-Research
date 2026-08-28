#pragma once

#include "imgui.h"
#include "core/memory/memory.hpp"
#include "core/renderer/sdl_renderer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace menu
{
    // The worker thread reads one immutable copy of the current controls. Only
    // Local Radar needs a GameSnapshot now that browser publication is removed.
    struct RuntimeConfig
    {
        bool espEnabled = true;
        bool espWeapon = true;
        bool espFlashIndicator = false;
        bool antiFlash = false;
        bool espViewAngle = true;
        bool espWallCheck = true;
        bool espSkeleton = true;
        bool grenadeESP = false;
        bool droppedWeaponESP = false;

        bool localRadarEnabled = false;
        bool localRadarShowNames = true;
        float localRadarAnchorX = 0.02f;
        float localRadarAnchorY = 0.08f;
        float localRadarSize = 0.32f;
        float localRadarMarkerSize = 12.0f;
        int radarRefreshRateHz = 20;

        [[nodiscard]] bool radarSnapshotEnabled() const noexcept
        {
            return localRadarEnabled;
        }
    };

    // These two values are the only feature controls exposed by the UI.
    inline bool espEnabled = true;
    inline bool localRadarEnabled = false;

    // ESP detail defaults remain internal so enabling ESP keeps the established
    // drawing behaviour without filling the interface with secondary options.
    inline bool espBox = true;
    inline bool espHealth = true;
    inline bool espDistance = true;
    inline bool espWeapon = true;
    inline bool espViewAngle = true;
    inline bool espViewAngleText = false;
    inline bool espFlashIndicator = false;
    inline bool espWallCheck = true;
    inline bool espSnaplines = false;
    inline bool espSkeleton = true;
    inline bool antiFlash = false;
    inline bool grenadeESP = false;
    inline bool droppedWeaponESP = false;

    // Existing ESP colours and line placement remain unchanged and are consumed
    // directly by esp.cpp.
    inline float espBoxColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    inline float espWallColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    inline float espDistanceColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    inline float espWeaponColor[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
    inline float espFlashNormalColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    inline float espFlashColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    inline float espSnaplinesColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    inline float espSkeletonColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    inline int snaplinesOrigin = 0;

    // Local Radar layout remains configurable through the existing settings
    // file even though the compact panel intentionally hides detail controls.
    inline bool localRadarShowNames = true;
    inline float localRadarAnchorX = 0.02f;
    inline float localRadarAnchorY = 0.08f;
    inline float localRadarSize = 0.32f;
    inline float localRadarMarkerSize = 12.0f;

    // Viewport auto-detection and fixed hotkeys are still used by the renderer.
    inline int viewportMode = 0;
    inline int menuToggleKey = VK_F4;
    inline int exitKey = VK_F9;
    inline bool isBindingKey = false;
    inline bool suppressHotkeysUntilRelease = false;

    // ImGui writes on the render thread while the sampling thread reads here.
    inline std::mutex configMutex;

    [[nodiscard]] inline RuntimeConfig buildRuntimeConfig()
    {
        RuntimeConfig config{};
        config.espEnabled = espEnabled;
        config.espWeapon = espWeapon;
        config.espFlashIndicator = espFlashIndicator;
        config.antiFlash = antiFlash;
        config.espViewAngle = espViewAngle;
        config.espWallCheck = espWallCheck;
        config.espSkeleton = espSkeleton;
        config.grenadeESP = grenadeESP;
        config.droppedWeaponESP = droppedWeaponESP;

        config.localRadarEnabled = localRadarEnabled;
        config.localRadarShowNames = localRadarShowNames;
        config.localRadarAnchorX = std::clamp(localRadarAnchorX, 0.0f, 1.0f);
        config.localRadarAnchorY = std::clamp(localRadarAnchorY, 0.0f, 1.0f);
        config.localRadarSize = std::clamp(localRadarSize, 0.18f, 0.65f);
        config.localRadarMarkerSize = std::clamp(
            localRadarMarkerSize,
            6.0f,
            24.0f);
        return config;
    }

    inline RuntimeConfig runtimeConfigSnapshot = buildRuntimeConfig();

    [[nodiscard]] inline RuntimeConfig getRuntimeConfig()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        return runtimeConfigSnapshot;
    }

    inline void publishRuntimeConfig()
    {
        const RuntimeConfig updated = buildRuntimeConfig();
        std::lock_guard<std::mutex> lock(configMutex);
        runtimeConfigSnapshot = updated;
    }

    // The settings path is kept stable so existing Local Radar placement and
    // viewport choices continue to work after the interface redesign.
    [[nodiscard]] inline std::filesystem::path persistentSettingsPath()
    {
        std::array<wchar_t, 32768> localAppData{};
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path directory =
            length > 0 && length < localAppData.size()
                ? std::filesystem::path(
                    std::wstring_view(localAppData.data(), length)) /
                    L"AegisCS2"
                : std::filesystem::temp_directory_path() / L"AegisCS2";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / L"settings-v1.ini";
    }

    [[nodiscard]] inline int readPersistentInt(
        const wchar_t* key,
        const int fallback,
        const std::filesystem::path& path)
    {
        return static_cast<int>(GetPrivateProfileIntW(
            L"settings",
            key,
            fallback,
            path.c_str()));
    }

    [[nodiscard]] inline float readPersistentFloat(
        const wchar_t* key,
        const float fallback,
        const std::filesystem::path& path)
    {
        std::array<wchar_t, 64> fallbackText{};
        std::array<wchar_t, 64> value{};
        swprintf_s(fallbackText.data(), fallbackText.size(), L"%.6f", fallback);
        GetPrivateProfileStringW(
            L"settings",
            key,
            fallbackText.data(),
            value.data(),
            static_cast<DWORD>(value.size()),
            path.c_str());
        wchar_t* end = nullptr;
        const float parsed = std::wcstof(value.data(), &end);
        return end != value.data() && std::isfinite(parsed)
            ? parsed
            : fallback;
    }

    inline void loadPersistentSettings()
    {
        const std::filesystem::path path = persistentSettingsPath();
        if (readPersistentInt(L"schema", 0, path) == 1) {
            viewportMode = std::clamp(
                readPersistentInt(L"viewport_mode", viewportMode, path),
                0,
                3);
            localRadarShowNames = readPersistentInt(
                L"local_radar_names",
                localRadarShowNames ? 1 : 0,
                path) != 0;
            localRadarAnchorX = std::clamp(
                readPersistentFloat(
                    L"local_radar_anchor_x",
                    localRadarAnchorX,
                    path),
                0.0f,
                1.0f);
            localRadarAnchorY = std::clamp(
                readPersistentFloat(
                    L"local_radar_anchor_y",
                    localRadarAnchorY,
                    path),
                0.0f,
                1.0f);
            localRadarSize = std::clamp(
                readPersistentFloat(
                    L"local_radar_size",
                    localRadarSize,
                    path),
                0.18f,
                0.65f);
            localRadarMarkerSize = std::clamp(
                readPersistentFloat(
                    L"local_radar_marker_size",
                    localRadarMarkerSize,
                    path),
                6.0f,
                24.0f);
        }
        publishRuntimeConfig();
    }

    inline void writePersistentValue(
        const wchar_t* key,
        const std::wstring_view value,
        const std::filesystem::path& path)
    {
        const std::wstring owned(value);
        WritePrivateProfileStringW(
            L"settings",
            key,
            owned.c_str(),
            path.c_str());
    }

    inline void savePersistentSettings()
    {
        const std::filesystem::path path = persistentSettingsPath();
        writePersistentValue(L"schema", L"1", path);
        writePersistentValue(
            L"viewport_mode",
            std::to_wstring(std::clamp(viewportMode, 0, 3)),
            path);
        writePersistentValue(
            L"local_radar_names",
            localRadarShowNames ? L"1" : L"0",
            path);

        const auto writeFloat = [&path](
            const wchar_t* key,
            const float value) {
            std::array<wchar_t, 64> text{};
            swprintf_s(text.data(), text.size(), L"%.6f", value);
            writePersistentValue(key, text.data(), path);
        };
        writeFloat(L"local_radar_anchor_x", localRadarAnchorX);
        writeFloat(L"local_radar_anchor_y", localRadarAnchorY);
        writeFloat(L"local_radar_size", localRadarSize);
        writeFloat(L"local_radar_marker_size", localRadarMarkerSize);
    }

    // Hotkeys are fixed in the compact interface, but the renderer still uses
    // this release check to prevent a key press from leaking into the game.
    [[nodiscard]] inline bool ConfiguredHotkeysReleased()
    {
        const int keys[] = { menuToggleKey, exitKey };
        for (const int key : keys) {
            if (key > 0 && key <= 0xFF &&
                (GetAsyncKeyState(key) & 0x8000)) {
                return false;
            }
        }
        return true;
    }

    // Draws a complete toggle card without ImGui's checkbox glyph. The entire
    // rounded card is clickable and the thumb animates between its two states.
    inline void renderSwitchCard(
        const char* id,
        const char* label,
        const ImVec4 accent,
        bool& enabled)
    {
        const float dpiScale = sdl_renderer::getDpiScale();
        const float cardHeight = 62.0f * dpiScale;
        const float rounding = 12.0f * dpiScale;
        const float trackWidth = 50.0f * dpiScale;
        const float trackHeight = 28.0f * dpiScale;
        const float padding = 18.0f * dpiScale;
        const ImVec2 minimum = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, cardHeight);
        const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);

        const ImGuiID animationId = ImGui::GetID(id);
        ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        if (ImGui::IsItemClicked()) {
            enabled = !enabled;
        }

        ImGuiStorage* storage = ImGui::GetStateStorage();
        float animation = storage->GetFloat(
            animationId,
            enabled ? 1.0f : 0.0f);
        const float target = enabled ? 1.0f : 0.0f;
        animation += (target - animation) * std::clamp(
            ImGui::GetIO().DeltaTime * 14.0f,
            0.0f,
            1.0f);
        storage->SetFloat(animationId, animation);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec4 base = held
            ? ImVec4(0.090f, 0.115f, 0.155f, 1.0f)
            : hovered
                ? ImVec4(0.072f, 0.095f, 0.132f, 1.0f)
                : ImVec4(0.052f, 0.070f, 0.102f, 1.0f);
        drawList->AddRectFilled(
            minimum,
            maximum,
            ImGui::GetColorU32(base),
            rounding);
        drawList->AddRect(
            minimum,
            maximum,
            ImGui::GetColorU32(
                enabled
                    ? ImVec4(accent.x, accent.y, accent.z, 0.82f)
                    : ImVec4(0.17f, 0.21f, 0.28f, 1.0f)),
            rounding,
            0,
            dpiScale);
        drawList->AddRectFilled(
            minimum,
            ImVec2(minimum.x + 4.0f * dpiScale, maximum.y),
            ImGui::GetColorU32(
                ImVec4(accent.x, accent.y, accent.z, enabled ? 1.0f : 0.30f)),
            rounding,
            ImDrawFlags_RoundCornersLeft);

        const ImVec2 textSize = ImGui::CalcTextSize(label);
        drawList->AddText(
            ImVec2(
                minimum.x + padding,
                minimum.y + (cardHeight - textSize.y) * 0.5f),
            ImGui::GetColorU32(ImVec4(0.93f, 0.96f, 1.0f, 1.0f)),
            label);

        const ImVec2 trackMin(
            maximum.x - padding - trackWidth,
            minimum.y + (cardHeight - trackHeight) * 0.5f);
        const ImVec2 trackMax(
            trackMin.x + trackWidth,
            trackMin.y + trackHeight);
        const ImVec4 offTrack(0.17f, 0.21f, 0.28f, 1.0f);
        const ImVec4 track(
            offTrack.x + (accent.x - offTrack.x) * animation,
            offTrack.y + (accent.y - offTrack.y) * animation,
            offTrack.z + (accent.z - offTrack.z) * animation,
            1.0f);
        drawList->AddRectFilled(
            trackMin,
            trackMax,
            ImGui::GetColorU32(track),
            trackHeight * 0.5f);

        const float thumbStart = trackMin.x + trackHeight * 0.5f;
        const float thumbEnd = trackMax.x - trackHeight * 0.5f;
        const ImVec2 thumb(
            thumbStart + (thumbEnd - thumbStart) * animation,
            (trackMin.y + trackMax.y) * 0.5f);
        drawList->AddCircleFilled(
            thumb,
            10.0f * dpiScale,
            ImGui::GetColorU32(ImVec4(0.97f, 0.98f, 1.0f, 1.0f)));
    }

    // The native panel contains exactly two controls and ignores any old saved
    // ImGui size, so rebuilding immediately produces the new compact layout.
    inline void render()
    {
        if (!sdl_renderer::menuVisible) {
            return;
        }

        if (!memory::WritesAllowed()) {
            antiFlash = false;
        }

        const float dpiScale = sdl_renderer::getDpiScale();
        const float margin = std::max(8.0f, 16.0f * dpiScale);
        const float windowWidth = std::min(
            360.0f * dpiScale,
            std::max(1.0f, static_cast<float>(WIDTH) - margin * 2.0f));
        const float windowHeight = std::min(
            166.0f * dpiScale,
            std::max(1.0f, static_cast<float>(HEIGHT) - margin * 2.0f));

        ImGui::SetNextWindowPos(
            ImVec2(
                (static_cast<float>(WIDTH) - windowWidth) * 0.5f,
                (static_cast<float>(HEIGHT) - windowHeight) * 0.5f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(windowWidth, windowHeight),
            ImGuiCond_Always);

        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(14.0f * dpiScale, 14.0f * dpiScale));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ItemSpacing,
            ImVec2(10.0f * dpiScale, 10.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * dpiScale);
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4(0.025f, 0.034f, 0.052f, 0.985f));

        ImGui::Begin(
            "Aegis Controls##TwoFeaturePanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);

        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        sdl_renderer::setInteractiveRect(
            position.x,
            position.y,
            size.x,
            size.y);

        renderSwitchCard(
            "##EspSwitch",
            "ESP",
            ImVec4(0.10f, 0.74f, 0.88f, 1.0f),
            espEnabled);
        renderSwitchCard(
            "##LocalRadarSwitch",
            "Local Radar",
            ImVec4(0.55f, 0.38f, 0.96f, 1.0f),
            localRadarEnabled);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        publishRuntimeConfig();
    }
}
