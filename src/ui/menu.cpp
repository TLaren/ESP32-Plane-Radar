#include "ui/menu.h"

#include "config.h"

#if PLANE_RADAR_TOUCH_UI

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

#include "config.h"
#include "hardware/cyd_extras.h"
#include "hardware/display.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"

namespace ui::menu {
namespace {

// 240x320 portrait layout. Each row: label on top, [-] value [+] below.
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kFg = 0xFFFF;
constexpr uint16_t kDim = config::kColorLabelGreen;  // section labels
constexpr uint16_t kBtn = 0x4A69;   // grey button face (was 0x2945, too dark)
constexpr int kStep = 10;

struct Box {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

#if defined(BOARD_JC3636)
// Round 360: screen brightness + location only (no LED, no light sensor),
// everything kept inside the circle. kHasLedRows=false hides the CYD rows.
constexpr bool kHasLedRows = false;
constexpr int kCx = 180;   // centre column
constexpr int kTitleY = 42;
constexpr int kRuleY = 70;
constexpr int kScrLabelY = 92;
constexpr Box kScrMinus{70, 112, 60, 40};
constexpr Box kScrPlus{230, 112, 60, 40};
constexpr Box kAuto{0, 0, 0, 0};
constexpr Box kLedMinus{0, 0, 0, 0};
constexpr Box kLedPlus{0, 0, 0, 0};
constexpr int kLedLabelY = 0;
constexpr int kLocY = 176;
// Bottom row: [Reset Wi-Fi] [Done], kept inside the circle.
constexpr Box kWifi{62, 240, 114, 42};
constexpr Box kDone{184, 240, 114, 42};
#else
constexpr bool kHasLedRows = true;
constexpr int kCx = 120;
constexpr int kTitleY = 6;
constexpr int kRuleY = 32;
constexpr int kScrLabelY = 40;
constexpr Box kScrMinus{8, 58, 56, 36};
constexpr Box kScrPlus{176, 58, 56, 36};
constexpr Box kAuto{8, 100, 224, 32};
constexpr Box kLedMinus{8, 162, 56, 36};
constexpr Box kLedPlus{176, 162, 56, 36};
constexpr int kLedLabelY = 144;
constexpr int kLocY = 206;
// Bottom row: [Reset Wi-Fi] [Done].
constexpr Box kWifi{8, 276, 108, 38};
constexpr Box kDone{124, 276, 108, 38};
#endif

bool s_open = false;

void button(const Box& b, const char* text, uint16_t face, uint16_t ink) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, face);
  tft.setTextColor(ink, face);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, b.x + b.w / 2, b.y + b.h / 2);
}

/** Value between a row's [-] and [+] buttons. */
void value(const Box& minus, const Box& plus, const char* text) {
  const int x0 = minus.x + minus.w + 2;
  tft.fillRect(x0, minus.y, plus.x - 2 - x0, minus.h, kBg);
  tft.setFont(&fonts::Font4);
  tft.setTextColor(config::kColorUiYellow, kBg);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, kCx, minus.y + minus.h / 2);
}

void drawValues() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", cyd::screenPct());
  value(kScrMinus, kScrPlus, buf);
  if (!kHasLedRows) return;
  tft.setFont(&fonts::Font2);
  button(kAuto, cyd::autoBrightness() ? "Auto (light sensor): ON" : "Auto (light sensor): OFF",
         cyd::autoBrightness() ? config::kColorUiYellow : kBtn,
         cyd::autoBrightness() ? config::kTextOnYellow : kFg);
  snprintf(buf, sizeof(buf), cyd::ledPct() == 0 ? "off" : "%d%%", cyd::ledPct());
  value(kLedMinus, kLedPlus, buf);
}

void drawAll() {
  tft.fillScreen(kBg);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(kFg, kBg);
  tft.drawString("SETTINGS", kCx, kTitleY);
  tft.drawFastHLine(kCx - 112, kRuleY, 224, kDim);

  tft.setFont(&fonts::Font2);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(kDim, kBg);
  tft.drawString("SCREEN BRIGHTNESS", kCx, kScrLabelY);
  if (kHasLedRows) tft.drawString("LED BRIGHTNESS", kCx, kLedLabelY);

  tft.setFont(&fonts::Font4);
  button(kScrMinus, "-", kBtn, kFg);
  button(kScrPlus, "+", kBtn, kFg);
  if (kHasLedRows) {
    button(kLedMinus, "-", kBtn, kFg);
    button(kLedPlus, "+", kBtn, kFg);
  }

  // Location: shown here, changed on the web page (no keypad on a 2.4" panel).
  char buf[48];
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(kDim, kBg);
  tft.drawString("LOCATION", kCx, kLocY);
  tft.setTextColor(kFg, kBg);
  snprintf(buf, sizeof(buf), "%.6f, %.6f", services::location::lat(),
           services::location::lon());
  tft.drawString(buf, kCx, kLocY + 16);
  tft.setTextColor(kDim, kBg);
  snprintf(buf, sizeof(buf), "change at http://%s", WiFi.localIP().toString().c_str());
  tft.drawString(buf, kCx, kLocY + 34);

  tft.setFont(&fonts::Font4);
  button(kDone, "Done", config::kColorUiYellow, config::kTextOnYellow);
  tft.setFont(&fonts::Font2);
  button(kWifi, "Reset Wi-Fi", kBtn, kFg);
  drawValues();
}

}  // namespace

bool isOpen() { return s_open; }

void open() {
  s_open = true;
  Serial.println("menu: open");
  drawAll();
}

bool tap(int x, int y) {
  if (!s_open) return false;
  if (kWifi.hit(x, y)) {
    // Same confirm screen as the BOOT long-press: a second tap within 5 s wipes
    // Wi-Fi + location and reboots into the setup network; otherwise back here.
    Serial.println("menu: Reset Wi-Fi tapped, asking to confirm");
    if (confirmWifiResetByTouch()) {
      Serial.println("menu: Wi-Fi reset confirmed");
      wifiResetCredentialsAndReboot();
    }
    Serial.println("menu: Wi-Fi reset cancelled");
    drawAll();
    return true;
  }
  if (kDone.hit(x, y)) {
    cyd::saveSettings();
    s_open = false;
    tft.fillScreen(kBg);
    Serial.println("menu: closed");
    return false;
  }
  if (kScrMinus.hit(x, y)) cyd::setScreenPct(cyd::screenPct() - kStep);
  else if (kScrPlus.hit(x, y)) cyd::setScreenPct(cyd::screenPct() + kStep);
  else if (kHasLedRows && kAuto.hit(x, y)) cyd::setAutoBrightness(!cyd::autoBrightness());
  else if (kHasLedRows && kLedMinus.hit(x, y)) cyd::setLedPct(cyd::ledPct() - kStep);
  else if (kHasLedRows && kLedPlus.hit(x, y)) cyd::setLedPct(cyd::ledPct() + kStep);
  else return true;
  drawValues();
  return true;
}

}  // namespace ui::menu

#endif
