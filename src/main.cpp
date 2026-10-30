/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#if defined(BOARD_CYD24)
#include "hardware/cyd_extras.h"
#include "ui/menu.h"
#endif
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

#if defined(BOARD_CYD24)
// Touch screen (calibrated, screen coordinates):
//   menu open                  -> menu handles it (Done closes, radar returns)
//   top-left RANGE corner      -> next range (same as a BOOT tap)
//   top-right MENU corner      -> settings menu
//   on the radar               -> select the plane nearest the tap (empty = clear)
//   info strip below the radar -> clear the selection
// One action per touch: re-arms only after the panel reads "released" for
// ~100 ms, because a resistive panel drops out briefly under a light finger.
constexpr int kRangeCornerW = 70;
constexpr int kRangeCornerH = 50;
constexpr int kRadarSizePx = 240;

void onScreenTap(int x, int y) {
  Serial.printf("touch: tap %d,%d\n", x, y);
  if (ui::menu::isOpen()) {
    if (!ui::menu::tap(x, y)) {
      showRadarIfConnected();
    }
    return;
  }
  if (!g_radar_visible) {
    return;
  }
  if (x < kRangeCornerW && y < kRangeCornerH) {
    onRangeTap();
  } else if (x >= kRadarSizePx - kRangeCornerW && x < kRadarSizePx &&
             y < kRangeCornerH) {
    ui::menu::open();
  } else if (y < kRadarSizePx && x < kRadarSizePx) {
    ui::radarSelectAt(x, y);
  } else {
    ui::radarClearSelection();
  }
}

void handleTouch() {
  static unsigned long s_last_poll_ms = 0;
  static bool s_armed = true;
  static uint8_t s_up_polls = 0;
  if (millis() - s_last_poll_ms < 25) {
    return;
  }
  s_last_poll_ms = millis();
  int32_t x = 0;
  int32_t y = 0;
  const bool down = tft.getTouch(&x, &y) > 0;
  if (down) {
    s_up_polls = 0;
    if (s_armed) {
      s_armed = false;
      onScreenTap(static_cast<int>(x), static_cast<int>(y));
    }
  } else if (!s_armed && ++s_up_polls >= 4) {
    s_armed = true;
  }
}
#endif

// ADS-B fetch runs on its own task: the HTTPS request blocks for ~1-2 s, and
// keeping it off the main loop lets the radar keep redrawing (dead-reckoned) at
// 4 Hz throughout. The task publishes into the shared aircraft buffer under a
// lock; the render loop reads a snapshot.
void adsbFetchTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      services::adsb::fetchUpdate(services::location::lat(),
                                  services::location::lon(),
                                  ui::radar::fetchRadiusKm());
    }
    vTaskDelay(pdMS_TO_TICKS(config::kAdsbFetchIntervalMs));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
#if defined(BOARD_CYD24)
  touchInit();
  cyd::init();
#endif
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // Start the background ADS-B fetch (pinned to core 0, away from the render
  // loop on core 1). It checks Wi-Fi state each cycle.
  xTaskCreatePinnedToCore(adsbFetchTask, "adsb", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  handleBootButton();
#if defined(BOARD_CYD24)
  handleTouch();
  cyd::loop();
#endif
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_redraw_ms >= config::kRadarRedrawIntervalMs
#if defined(BOARD_CYD24)
               && !ui::menu::isOpen()  // settings page owns the screen
#endif
    ) {
      // Redraw at 4 Hz with dead-reckoned positions; the ADS-B fetch runs on
      // its own task (adsbFetchTask).
      g_last_redraw_ms = millis();
      ui::radarDisplayRefreshAircraft();
    }
  }

  delay(10);
}
