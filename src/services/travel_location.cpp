#include "services/travel_location.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/geo.h"
#include "services/net_lock.h"
#include "services/nmea.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"

namespace services::travel {

namespace {

using location::Source;

// ---------------------------------------------------------------- Wi-Fi lookup

struct Ap {
  uint8_t mac[6];
  int rssi;
};

constexpr size_t kMaxScanAps = 40;
constexpr size_t kMaxSentAps = 12;

/**
 * Approximate position from the Wi-Fi networks in range, via beaconDB (a free,
 * community geolocation database; Google-style request). Only BSSIDs and signal
 * strengths are sent. Leaves out: locally administered MACs (phone hotspots,
 * mesh/guest copies), the hotspot we are on, and networks named *_nomap.
 */
bool lookupFromWifi(double* lat, double* lon) {
  uint8_t own[6] = {};
  const uint8_t* cur = WiFi.BSSID();
  if (cur != nullptr) {
    memcpy(own, cur, 6);
  }

  const int found = WiFi.scanNetworks(false, false);
  if (found < 2) {
    Serial.printf("travel: Wi-Fi lookup skipped (%d networks in range)\n", found);
    WiFi.scanDelete();
    return false;
  }

  Ap aps[kMaxScanAps];
  size_t n = 0;
  for (int i = 0; i < found && n < kMaxScanAps; ++i) {
    const uint8_t* b = WiFi.BSSID(i);
    if (b == nullptr || (b[0] & 0x02) != 0 || memcmp(b, own, 6) == 0) {
      continue;
    }
    if (WiFi.SSID(i).endsWith("_nomap")) {
      continue;
    }
    memcpy(aps[n].mac, b, 6);
    aps[n].rssi = WiFi.RSSI(i);
    ++n;
  }
  WiFi.scanDelete();

  if (n < 2) {
    Serial.printf("travel: Wi-Fi lookup skipped (%u usable networks)\n",
                  static_cast<unsigned>(n));
    return false;
  }
  std::sort(aps, aps + n, [](const Ap& a, const Ap& b) { return a.rssi > b.rssi; });
  if (n > kMaxSentAps) {
    n = kMaxSentAps;
  }

  String body = "{\"considerIp\":false,\"wifiAccessPoints\":[";
  for (size_t i = 0; i < n; ++i) {
    char item[80];
    snprintf(item, sizeof(item),
             "%s{\"macAddress\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"signalStrength\":%d}",
             i == 0 ? "" : ",", aps[i].mac[0], aps[i].mac[1], aps[i].mac[2],
             aps[i].mac[3], aps[i].mac[4], aps[i].mac[5], aps[i].rssi);
    body += item;
  }
  body += "]}";

  // The scan above needs no lock; only the TLS request takes turns with the
  // ADS-B fetch (heap, see net_lock.h), so planes are not held up for the scan.
  services::net::Guard guard(20000);
  if (!guard.ok()) {
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, config::kWifiLookupUrl)) {
    Serial.println("travel: Wi-Fi lookup http.begin failed");
    return false;
  }
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  if (code != 200) {
    Serial.printf("travel: Wi-Fi lookup HTTP %d\n", code);
    http.end();
    return false;
  }
  const String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    Serial.println("travel: Wi-Fi lookup: bad JSON");
    return false;
  }
  if (!doc["fallback"].isNull()) {
    Serial.println("travel: Wi-Fi lookup fell back to IP location, ignored");
    return false;
  }
  const float accuracy = doc["accuracy"] | 1.0e9f;
  if (doc["location"]["lat"].isNull() || doc["location"]["lng"].isNull() ||
      accuracy > config::kWifiLookupMaxAccuracyM) {
    Serial.printf("travel: Wi-Fi lookup too vague (%.0f m)\n",
                  static_cast<double>(accuracy));
    return false;
  }
  *lat = doc["location"]["lat"].as<double>();
  *lon = doc["location"]["lng"].as<double>();
  Serial.printf("travel: Wi-Fi lookup %.6f, %.6f (+/- %.0f m, %u networks)\n", *lat,
                *lon, static_cast<double>(accuracy), static_cast<unsigned>(n));
  return true;
}

// ----------------------------------------------------------------------- task

void travelTask(void*) {
  WiFiClient gps;
  char line[128];
  size_t line_len = 0;
  bool skipping = false;  // inside an over-long line: drop it up to the newline

  bool was_phone = false;
  unsigned long net_since = 0;
  unsigned long last_connect_try = 0;
  unsigned long last_lookup = 0;
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
      net_since = now;
      last_connect_try = 0;
      last_lookup = 0;
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
      if (last_connect_try == 0 || now - last_connect_try >= 10000) {
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
    } else if (now - net_since >= config::kWifiLookupFirstDelayMs &&
               (last_lookup == 0 ||
                now - last_lookup >= config::kWifiLookupIntervalMs)) {
      // --- fallback: no (fresh) phone GPS, so ask what the Wi-Fi around says ---
      last_lookup = now;
      double lat = 0.0;
      double lon = 0.0;
      if (lookupFromWifi(&lat, &lon)) {
        const bool first = !applied || applied_src != Source::WifiLookup;
        if (first ||
            geo::distanceKm(applied_lat, applied_lon, lat, lon) >= config::kTravelMinMoveKm) {
          location::setTravel(lat, lon, Source::WifiLookup);
          applied = true;
          applied_src = Source::WifiLookup;
          applied_lat = lat;
          applied_lon = lon;
          Serial.printf("travel: radar moved to %.6f, %.6f (Wi-Fi lookup)\n", lat, lon);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void poll() {
  // Created the first time the unit is on the phone hotspot, never at home:
  // a CYD has no PSRAM and the task's stack alone is ~10 KB of heap.
  static bool s_started = false;
  if (s_started || !wifiOnPhoneNetwork()) {
    return;
  }
  s_started = true;
  xTaskCreatePinnedToCore(travelTask, "travel", 10240, nullptr, 1, nullptr, 0);
}

}  // namespace services::travel
