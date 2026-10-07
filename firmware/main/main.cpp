/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>

#if CONFIG_STACKCHAN_WEBRTC_M0
#include <hal/webrtc/webrtc_m0.h>
#endif
#if CONFIG_STACKCHAN_WEBRTC_M1
#include <hal/webrtc/webrtc_m1.h>
#include <board.h>
#include <wifi_manager.h>
#include <esp_log.h>
#endif

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

#if CONFIG_STACKCHAN_WEBRTC_M0
    // M0: WebRTC interoperability probe. Runs instead of the normal firmware so
    // the working WebSocket path is left untouched. Never returns.
    {
        // Nothing else in this path brings the network up: normally
        // startXiaozhi() -> Application::Initialize() calls StartNetwork(), and
        // M0 skips all of that. Without this the wait below never ends.
        ESP_LOGW("M0", "starting the network...");
        Board::GetInstance().StartNetwork();

        // WifiManager is the interface the Aliyun protocol already uses to ask
        // whether the link is up.
        auto& wifi = WifiManager::GetInstance();
        ESP_LOGW("M0", "waiting for the network before probing WebRTC...");
        while (!wifi.IsConnected()) {
            GetHAL().feedTheDog();
            GetHAL().delay(500);
        }
        ESP_LOGW("M0", "network is up");
        WebRtcM0Run();
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().delay(1000);
        }
    }
#endif

#if CONFIG_STACKCHAN_WEBRTC_M1
    // M1: media-uplink probe. Runs instead of the normal firmware so the
    // working WebSocket path is untouched. Never returns.
    {
        ESP_LOGW("M1", "starting the network...");
        Board::GetInstance().StartNetwork();
        auto& wifi = WifiManager::GetInstance();
        ESP_LOGW("M1", "waiting for the network before starting the media probe...");
        while (!wifi.IsConnected()) {
            GetHAL().feedTheDog();
            GetHAL().delay(500);
        }
        ESP_LOGW("M1", "network is up");
        WebRtcM1Run();
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().delay(1000);
        }
    }
#endif

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake =
        GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
