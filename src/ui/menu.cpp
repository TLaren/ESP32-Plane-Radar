#include "ui/menu.h"

#if defined(BOARD_CYD24)

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

#include "config.h"
#include "hardware/cyd_extras.h"
#include "hardware/display.h"
#include "services/radar_location.h"

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

constexpr Box kScrMinus{8, 58, 56, 36};
constexpr Box kScrPlus{176, 58, 56, 36};
constexpr Box kAuto{8, 100, 224, 32};
constexpr Box kLedMinus{8, 162, 56, 36};
constexpr Box kLedPlus{176, 162, 56, 36};
constexpr Box kDone{8, 276, 224, 38};

bool s_open = false;

void button(const Box& b, const char* text, uint16_t face, uint16_t ink) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, face);
  tft.setTextColor(ink, face);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, b.x + b.w / 2, b.y + b.h / 2);
}

void value(int y, const char* text) {
  tft.fillRect(66, y, 108, 36, kBg);
  tft.setFont(&fonts::Font4);
  tft.setTextColor(config::kColorUiYellow, kBg);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, 120, y + 18);
}

void drawValues() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", cyd::screenPct());
  value(kScrMinus.y, buf);
  tft.setFont(&fonts::Font2);
  button(kAuto, cyd::autoBrightness() ? "Auto (light sensor): ON" : "Auto (light sensor): OFF",
         cyd::autoBrightness() ? config::kColorUiYellow : kBtn,
         cyd::autoBrightness() ? config::kTextOnYellow : kFg);
  snprintf(buf, sizeof(buf), cyd::ledPct() == 0 ? "off" : "%d%%", cyd::ledPct());
  value(kLedMinus.y, buf);
}

void drawAll() {
  tft.fillScreen(kBg);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(kFg, kBg);
  tft.drawString("SETTINGS", 120, 6);
  tft.drawFastHLine(8, 32, 224, kDim);

  tft.setFont(&fonts::Font2);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(kDim, kBg);
  tft.drawString("SCREEN BRIGHTNESS", 8, 40);
  tft.drawString("LED BRIGHTNESS", 8, 144);

  tft.setFont(&fonts::Font4);
  button(kScrMinus, "-", kBtn, kFg);
  button(kScrPlus, "+", kBtn, kFg);
  button(kLedMinus, "-", kBtn, kFg);
  button(kLedPlus, "+", kBtn, kFg);

  // Location: shown here, changed on the web page (no keypad on a 2.4" panel).
  char buf[48];
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(kDim, kBg);
  tft.drawString("LOCATION", 8, 206);
  tft.setTextColor(kFg, kBg);
  snprintf(buf, sizeof(buf), "%.6f, %.6f", services::location::lat(),
           services::location::lon());
  tft.drawString(buf, 8, 222);
  tft.setTextColor(kDim, kBg);
  snprintf(buf, sizeof(buf), "change at http://%s", WiFi.localIP().toString().c_str());
  tft.drawString(buf, 8, 240);

  tft.setFont(&fonts::Font4);
  button(kDone, "Done", config::kColorUiYellow, config::kTextOnYellow);
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
  if (kDone.hit(x, y)) {
    cyd::saveSettings();
    s_open = false;
    tft.fillScreen(kBg);
    Serial.println("menu: closed");
    return false;
  }
  if (kScrMinus.hit(x, y)) cyd::setScreenPct(cyd::screenPct() - kStep);
  else if (kScrPlus.hit(x, y)) cyd::setScreenPct(cyd::screenPct() + kStep);
  else if (kAuto.hit(x, y)) cyd::setAutoBrightness(!cyd::autoBrightness());
  else if (kLedMinus.hit(x, y)) cyd::setLedPct(cyd::ledPct() - kStep);
  else if (kLedPlus.hit(x, y)) cyd::setLedPct(cyd::ledPct() + kStep);
  else return true;
  drawValues();
  return true;
}

}  // namespace ui::menu

#endif
