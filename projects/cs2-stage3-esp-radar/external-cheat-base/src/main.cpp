#include "features/esp.hpp"
#include "core/renderer/sdl_renderer.h"
#include "features/menu.hpp"
#include "features/local_radar/local_fixed_radar.hpp"
#include "core/diagnostics.hpp"
#include "core/performance_metrics.hpp"
#include "core/runtime_timing.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

// #define SHOW_CONSOLE

uint32_t WIDTH;
uint32_t HEIGHT;
uint32_t WINDOW_W;
uint32_t WINDOW_H;
int32_t VIEWPORT_X;
int32_t VIEWPORT_Y;
uint32_t VIEWPORT_W;
uint32_t VIEWPORT_H;

// The waiting state uses the same compact switch panel as the attached
// overlay. This prevents startup from showing the old connection dashboard
// before CS2 is detected, while still polling for the game in the background.
void renderWaitingScreen()
{
    menu::render();
}

namespace
{
    class ScopedHandle
    {
    public:
        explicit ScopedHandle(HANDLE handle = nullptr)
            : handle_(handle)
        {
        }

        ~ScopedHandle()
        {
            if (handle_) {
                CloseHandle(handle_);
            }
        }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        HANDLE get() const
        {
            return handle_;
        }

    private:
        HANDLE handle_ = nullptr;
    };

    bool hasArgument(
        int argc,
        char* argv[],
        const char* expected)
    {
        for (int index = 1; index < argc; ++index) {
            if (_stricmp(argv[index], expected) == 0) {
                return true;
            }
        }
        return false;
    }

    void showFatalError(const wchar_t* message)
    {
        diagnostics::log(message);
        MessageBoxW(
            nullptr,
            message,
            L"CS2 ESP",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }

}

int main(int argc, char* argv[])
{
    SetLastError(ERROR_SUCCESS);
    ScopedHandle instanceMutex(CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\CS2ExternalOverlay-76F5E6C2-235A-4AA0"));
    const DWORD mutexError = GetLastError();
    if (!instanceMutex.get()) {
        showFatalError(
            L"Unable to create the single-instance guard.");
        return -1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"CS2 ESP is already running.",
            L"CS2 ESP",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    memory::SetWritesAllowed(
        hasArgument(
            argc,
            argv,
            "--allow-memory-writes"));
    menu::loadPersistentSettings();

#ifndef SHOW_CONSOLE
    FreeConsole();
#endif

    while (sdl_renderer::running)
    {
        sdl_renderer::menuVisible = true;
        if (!sdl_renderer::initWaiting()) {
            showFatalError(
                L"Unable to initialize the overlay. Windows 10/11 with "
                L"per-monitor DPI awareness and a working SDL2.dll are required.");
            memory::Close();
            return -1;
        }
        if (!sdl_renderer::initImGui()) {
            showFatalError(L"Unable to initialize ImGui.");
            local_fixed_radar::reset();
            sdl_renderer::destroy();
            memory::Close();
            return -1;
        }

        DWORD lastCheckTime = 0;
        bool gameFound = false;
        while (sdl_renderer::running && !gameFound)
        {
            sdl_renderer::pollEvents();

            const DWORD currentTime = GetTickCount();
            if (currentTime - lastCheckTime >= 3000 ||
                lastCheckTime == 0) {
                lastCheckTime = currentTime;
                if (esp::init()) {
                    gameFound = true;
                    break;
                }
            }

            if (!sdl_renderer::beginFrame()) {
                break;
            }
            sdl_renderer::newFrameImGui();
            renderWaitingScreen();
            sdl_renderer::renderImGui();
            sdl_renderer::endFrame();
            Sleep(16);
        }

        sdl_renderer::shutdownImGui();
        local_fixed_radar::reset();
        sdl_renderer::destroy();
        if (!sdl_renderer::running) {
            break;
        }
        if (!gameFound ||
            !sdl_renderer::init(
                L"Counter-Strike 2",
                static_cast<DWORD>(esp::pID)) ||
            !sdl_renderer::initImGui()) {
            diagnostics::log(
                L"Game attach failed or raced with shutdown; retrying.");
            sdl_renderer::shutdownImGui();
            local_fixed_radar::reset();
            sdl_renderer::destroy();
            esp::clearRuntimeState();
            memory::Close();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
            continue;
        }

        // ESP follows the display refresh rate. Local Radar samples its compact
        // GameSnapshot at 20 Hz and pauses while CS2 is not in the foreground.
        std::atomic<bool> dataRunning{true};
        performance_metrics::resetSession();
        memory::ResetReadMetrics();
        std::thread dataThread([&dataRunning]() {
            auto nextDataTime = std::chrono::steady_clock::now();
            int appliedSamplingRate = 0;
            int appliedThreadPriority = 0x7FFFFFFF;
            bool stateClearedWhileInactive = false;

            while (dataRunning.load(std::memory_order_relaxed)) {
                const menu::RuntimeConfig config =
                    menu::getRuntimeConfig();
                const bool gameForeground =
                    sdl_renderer::isGameForeground();
                const int desiredThreadPriority = THREAD_PRIORITY_BELOW_NORMAL;
                if (desiredThreadPriority != appliedThreadPriority &&
                    SetThreadPriority(
                        GetCurrentThread(),
                        desiredThreadPriority)) {
                    appliedThreadPriority = desiredThreadPriority;
                }

                const bool radarSamplingEnabled =
                    config.radarSnapshotEnabled();
                const int radarRate = radarSamplingEnabled
                    ? runtime_timing::radarSamplingRate(
                        runtime_timing::RadarDemand{
                            config.localRadarEnabled,
                            gameForeground
                        })
                    : 0;
                performance_metrics::radarRateHz.store(
                    radarRate,
                    std::memory_order_relaxed);

                // Both visible features are foreground overlays. Clear cached
                // game state once while CS2 is inactive and avoid background RPM.
                if (!gameForeground) {
                    if (!stateClearedWhileInactive) {
                        esp::clearRuntimeState();
                        stateClearedWhileInactive = true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(25));
                    nextDataTime = std::chrono::steady_clock::now();
                    continue;
                }
                stateClearedWhileInactive = false;

                const runtime_timing::SamplingDemand samplingDemand{
                    false,
                    config.espEnabled ||
                        config.grenadeESP ||
                        config.droppedWeaponESP,
                    config.antiFlash,
                    radarRate > 0 ? radarRate : 20
                };
                const int samplingRate =
                    runtime_timing::dataSamplingRate(
                        sdl_renderer::getTargetRefreshRate(),
                        samplingDemand);
                if (samplingRate != appliedSamplingRate) {
                    appliedSamplingRate = samplingRate;
                    performance_metrics::samplingRateHz.store(
                        samplingRate,
                        std::memory_order_relaxed);
                    nextDataTime = std::chrono::steady_clock::now();
                }
                const auto dataInterval =
                    runtime_timing::intervalForRate(samplingRate);

                menu::RuntimeConfig samplingConfig = config;
                samplingConfig.radarRefreshRateHz =
                    std::max(1, radarRate);
                const auto samplingStarted =
                    std::chrono::steady_clock::now();
                esp::updateEntities(samplingConfig);
                performance_metrics::samplingDuration.record(
                    std::chrono::steady_clock::now() - samplingStarted);

                nextDataTime += dataInterval;
                const auto now = std::chrono::steady_clock::now();
                if (nextDataTime > now) {
                    std::this_thread::sleep_until(nextDataTime);
                } else {
                    // Back off when RPM work exceeds its budget so the worker
                    // cannot monopolize a CPU core.
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                    performance_metrics::missedSamplingDeadlines.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    nextDataTime = now + dataInterval;
                }
            }
        });

        auto nextRenderTime = std::chrono::steady_clock::now();
        while (sdl_renderer::running &&
               !sdl_renderer::isGameDisconnected())
        {
            const auto renderStarted = std::chrono::steady_clock::now();
            sdl_renderer::pollEvents();
            sdl_renderer::updateWindowPosition();
            if (!sdl_renderer::isGameVisible()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(25));
                nextRenderTime = std::chrono::steady_clock::now();
                continue;
            }

            if (!sdl_renderer::beginFrame()) {
                break;
            }
            sdl_renderer::newFrameImGui();

            if (menu::espEnabled ||
                menu::grenadeESP ||
                menu::droppedWeaponESP) {
                esp::render();
            }
            const menu::RuntimeConfig renderConfig =
                menu::getRuntimeConfig();
            if (renderConfig.localRadarEnabled) {
                const esp::GameSnapshot snapshot =
                    esp::getGameSnapshot();
                if (snapshot) {
                    local_fixed_radar::render(
                        *snapshot,
                        local_fixed_radar::RenderConfig{
                            renderConfig.localRadarAnchorX,
                            renderConfig.localRadarAnchorY,
                            renderConfig.localRadarSize,
                            renderConfig.localRadarMarkerSize,
                            renderConfig.localRadarShowNames
                        });
                }
            } else {
                local_fixed_radar::reset();
            }
            menu::render();

            sdl_renderer::renderImGui();
            sdl_renderer::endFrame();
            performance_metrics::renderCpuDuration.record(
                std::chrono::steady_clock::now() - renderStarted);

            const int refreshRate = std::max(
                60,
                sdl_renderer::getTargetRefreshRate());
            const auto renderInterval =
                std::chrono::nanoseconds(
                    1000000000LL / refreshRate);
            nextRenderTime += renderInterval;
            const auto now = std::chrono::steady_clock::now();
            if (nextRenderTime > now) {
                std::this_thread::sleep_until(nextRenderTime);
            } else {
                performance_metrics::missedRenderDeadlines.fetch_add(
                    1,
                    std::memory_order_relaxed);
                nextRenderTime = now;
            }
        }

        dataRunning.store(false, std::memory_order_relaxed);
        dataThread.join();
        sdl_renderer::shutdownImGui();
        local_fixed_radar::reset();
        sdl_renderer::destroy();
        esp::clearRuntimeState();
        memory::Close();

        if (sdl_renderer::running) {
            diagnostics::log(
                L"CS2 disconnected; returning to the waiting screen.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
        }
    }

    menu::savePersistentSettings();
    memory::Close();
    return 0;
}
