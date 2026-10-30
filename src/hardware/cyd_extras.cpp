#include "hardware/cyd_extras.h"

#if defined(BOARD_CYD24)

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"

namespace cyd {
namespace {

// RGB LED: common anode, so each pin is active LOW. PWM channels 0-2; the
// backlight's Light_PWM uses channel 7 (its timer is shared with 6 -- avoid).
constexpr int kLedPins[3] = {config::kLedPinR, config::kLedPinG, config::kLedPinB};
constexpr int kLedChannels[3] = {0, 1, 2};
constexpr int kLedPwmHz = 5000;

constexpr char kPrefs[] = "ui";
int s_screen_pct = 100;
int s_led_pct = 30;
bool s_auto = false;

float s_light = 1.0f;          // 0.1 (dark) .. 1.0 (bright); 1.0 when auto is off
unsigned long s_last_ldr_ms = 0;
unsigned long s_last_ldr_log_ms = 0;
float s_ldr_smooth = -1.0f;

unsigned long s_seen_update_ms = 0;
unsigned long s_green_since = 0;
unsigned long s_last_alert_check_ms = 0;
bool s_alert = false;
uint32_t s_led_shown = 0xFFFFFFFF;

void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t packed = (r << 16) | (g << 8) | b;
  if (packed == s_led_shown) return;
  s_led_shown = packed;
  const uint8_t v[3] = {r, g, b};
  for (int i = 0; i < 3; ++i) {
    ledcWrite(kLedChannels[i], 255 - v[i]);  // active low
  }
}

/** Colour at the user's LED brightness, dimmed with the room when auto is on. */
void ledColor(bool r, bool g, bool b) {
  const int level = static_cast<int>(lroundf(255.0f * s_led_pct / 100.0f * s_light));
  const uint8_t l = static_cast<uint8_t>(std::max(0, std::min(255, level)));
  ledWrite(r ? l : 0, g ? l : 0, b ? l : 0);
}

void applyScreen() {
  const float pct = s_screen_pct * s_light;
  tft.setBrightness(static_cast<uint8_t>(std::max(8.0f, std::min(255.0f, pct * 2.55f))));
}

/** Any airborne plane within kAlertDistKm and below kAlertAltFt. */
bool proximityAlert() {
  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  const size_t n = services::adsb::snapshotAircraft(planes, services::adsb::kMaxAircraft, nullptr);
  const float clat = static_cast<float>(services::location::lat());
  const float clon = static_cast<float>(services::location::lon());
  const float coslat = cosf(clat * 0.01745329252f);
  for (size_t i = 0; i < n; ++i) {
    const char* alt = planes[i].alt;
    if (alt[0] == '\0' || strcmp(alt, "GND") == 0) continue;
    const int ft = atoi(alt);
    if (ft >= config::kAlertAltFt) continue;
    const float dx = (planes[i].lon - clon) * 111.0f * coslat;
    const float dy = (planes[i].lat - clat) * 111.0f;
    if (dx * dx + dy * dy <= config::kAlertDistKm * config::kAlertDistKm) {
      return true;
    }
  }
  return false;
}

void updateLight(unsigned long now) {
  if (now - s_last_ldr_ms < 500) return;
  s_last_ldr_ms = now;
  const int raw = analogRead(config::kLdrPin);
  s_ldr_smooth = s_ldr_smooth < 0 ? raw : s_ldr_smooth * 0.8f + raw * 0.2f;
  if (now - s_last_ldr_log_ms >= 10000) {
    s_last_ldr_log_ms = now;
    Serial.printf("ldr: raw %d (smoothed %.0f)%s\n", raw, s_ldr_smooth, s_auto ? "" : " [auto off]");
  }
  float light = 1.0f;
  if (s_auto) {
    // Map smoothed reading between the dark and bright calibration points.
    const float t = (s_ldr_smooth - config::kLdrDarkRaw) /
                    static_cast<float>(config::kLdrBrightRaw - config::kLdrDarkRaw);
    light = 0.1f + 0.9f * std::max(0.0f, std::min(1.0f, t));
  }
  if (fabsf(light - s_light) > 0.02f) {
    s_light = light;
    applyScreen();
    s_led_shown = 0xFFFFFFFF;  // re-apply the LED level too
  }
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kPrefs, true)) {
    s_screen_pct = prefs.getInt("scr", 100);
    s_led_pct = prefs.getInt("led", 30);
    s_auto = prefs.getBool("auto", false);
    prefs.end();
  }
  analogSetPinAttenuation(config::kLdrPin, ADC_0db);
  for (int i = 0; i < 3; ++i) {
    ledcSetup(kLedChannels[i], kLedPwmHz, 8);
    ledcAttachPin(kLedPins[i], kLedChannels[i]);
  }
  // Pin check: red, green, blue for 300 ms each at full, then off.
  Serial.println("led: test R, G, B");
  ledWrite(255, 0, 0); delay(300);
  ledWrite(0, 255, 0); delay(300);
  ledWrite(0, 0, 255); delay(300);
  ledWrite(0, 0, 0);
  applyScreen();
  Serial.printf("ui: screen %d%%, led %d%%, auto %s\n", s_screen_pct, s_led_pct,
                s_auto ? "on" : "off");
}

void loop() {
  const unsigned long now = millis();
  updateLight(now);

  if (now - s_last_alert_check_ms >= 1000) {
    s_last_alert_check_ms = now;
    const bool alert = proximityAlert();
    if (alert != s_alert) {
      Serial.printf("led: proximity alert %s\n", alert ? "ON" : "off");
      s_alert = alert;
    }
  }

  const unsigned long last_ok = services::adsb::lastUpdateMs();
  if (last_ok != 0 && last_ok != s_seen_update_ms) {
    s_seen_update_ms = last_ok;
    s_green_since = now;
  }
  const bool feed_bad = WiFi.status() != WL_CONNECTED ||
                        (last_ok == 0 ? now > 30000 : now - last_ok > 30000);

  if (s_alert) {
    ledColor(true, false, false);                 // solid red: low and close
  } else if (feed_bad) {
    ledColor(false, false, (now % 2000) < 150);   // blue blink: no WiFi / no data
  } else if (now - s_green_since < 80) {
    ledColor(false, true, false);                 // green blink: fresh data
  } else {
    ledColor(false, false, false);
  }
}

int screenPct() { return s_screen_pct; }
int ledPct() { return s_led_pct; }
bool autoBrightness() { return s_auto; }

void setScreenPct(int pct) {
  s_screen_pct = std::max(10, std::min(100, pct));
  applyScreen();
}

void setLedPct(int pct) {
  s_led_pct = std::max(0, std::min(100, pct));
  s_led_shown = 0xFFFFFFFF;
}

void setAutoBrightness(bool on) {
  s_auto = on;
  s_last_ldr_ms = 0;
  if (!on) {
    s_light = 1.0f;
    applyScreen();
  }
}

void saveSettings() {
  Preferences prefs;
  if (prefs.begin(kPrefs, false)) {
    prefs.putInt("scr", s_screen_pct);
    prefs.putInt("led", s_led_pct);
    prefs.putBool("auto", s_auto);
    prefs.end();
  }
  Serial.printf("ui: saved screen %d%%, led %d%%, auto %s\n", s_screen_pct, s_led_pct,
                s_auto ? "on" : "off");
}

}  // namespace cyd

#endif
