#include "services/travel_location.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/geo.h"
#include "services/nmea.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"

namespace services::travel {

namespace {

using location::Source;

// ----------------------------------------------------------------------- task

void travelTask(void*) {
  WiFiClient gps;
  char line[128];
  size_t line_len = 0;
  bool skipping = false;  // inside an over-long line: drop it up to the newline

  bool was_phone = false;
  unsigned long last_connect_try = 0;
  unsigned long last_eval = 0;

  nmea::Fix latest = {};
  bool have_fix = false;
  unsigned long fix_ms = 0;

  bool applied = false;
  Source applied_src = Source::Saved;
  double applied_lat = 0.0;
  double applied_lon = 0.0;

  for (;;) {
    const bool on_phone = wifiOnPhoneNetwork();

    if (!on_phone) {
      if (was_phone) {
        Serial.println("travel: left the phone hotspot, back to the saved location");
        gps.stop();
        location::clearTravel();
        was_phone = false;
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const unsigned long now = millis();
    if (!was_phone) {
      was_phone = true;
      last_connect_try = 0;
      last_eval = 0;
      have_fix = false;
      applied = false;
      applied_src = Source::Saved;
      line_len = 0;
      skipping = false;
      gps.stop();
      Serial.printf("travel: on the phone hotspot, GPS port %u (free heap %u)\n",
                    static_cast<unsigned>(phoneGpsPort()),
                    static_cast<unsigned>(ESP.getFreeHeap()));
    }

    // --- phone GPS: NMEA lines over TCP to the hotspot gateway (the phone) ---
    if (!gps.connected()) {
      if (line_len != 0 || skipping) {
        line_len = 0;
        skipping = false;
      }
      if (last_connect_try == 0 || now - last_connect_try >= 30000) {
        last_connect_try = now;
        const IPAddress gw = WiFi.gatewayIP();
        if (gps.connect(gw, phoneGpsPort(), 3000)) {
          gps.setNoDelay(true);
          Serial.printf("travel: phone GPS connected (%s:%u)\n", gw.toString().c_str(),
                        static_cast<unsigned>(phoneGpsPort()));
        }
      }
    }
    while (gps.connected() && gps.available() > 0) {
      const int c = gps.read();
      if (c < 0) {
        break;
      }
      if (c == '\n' || c == '\r') {
        if (!skipping && line_len > 0) {
          line[line_len] = '\0';
          nmea::Fix f;
          if (nmea::parseSentence(line, &f)) {
            latest = f;
            have_fix = true;
            fix_ms = millis();
          }
        }
        line_len = 0;
        skipping = false;
      } else if (!skipping) {
        if (line_len < sizeof(line) - 1) {
          line[line_len++] = static_cast<char>(c);
        } else {
          skipping = true;
          line_len = 0;
        }
      }
    }

    const bool gps_fresh = have_fix && (millis() - fix_ms) < config::kGpsStaleMs;

    // --- apply: first fix at once, then at most once a minute and >= 1 km away ---
    if (gps_fresh) {
      const bool first = !applied || applied_src != Source::PhoneGps;
      if (first || now - last_eval >= config::kTravelApplyIntervalMs) {
        last_eval = now;
        if (first || geo::distanceKm(applied_lat, applied_lon, latest.lat,
                                     latest.lon) >= config::kTravelMinMoveKm) {
          location::setTravel(latest.lat, latest.lon, Source::PhoneGps);
          applied = true;
          applied_src = Source::PhoneGps;
          applied_lat = latest.lat;
          applied_lon = latest.lon;
          Serial.printf("travel: radar moved to %.6f, %.6f (phone GPS)\n", latest.lat,
                        latest.lon);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void poll() {
  // Created the first time the unit is on the phone hotspot, never at home:
  // a CYD has no PSRAM and the task's stack alone is ~6 KB of heap.
  static bool s_started = false;
  static bool s_off_logged = false;
  if (s_started || !wifiOnPhoneNetwork()) {
    return;
  }
  if (!travelModeEnabled()) {
    if (!s_off_logged) {
      s_off_logged = true;
      Serial.println("travel: mode is OFF (setup page), staying on the saved location");
    }
    return;
  }
  s_started = true;
  xTaskCreatePinnedToCore(travelTask, "travel", 6144, nullptr, 1, nullptr, 0);
}

}  // namespace services::travel
