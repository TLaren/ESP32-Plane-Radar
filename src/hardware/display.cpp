#include <Arduino.h>
#include "hardware/display.h"

#include "config.h"

#include "hardware/display_font.h"

#if defined(BOARD_CYD24)
#include <Preferences.h>
#endif

LGFX tft;

void displayInit() {
  tft.init();
  tft.setRotation(config::kDisplayRotation);
  tft.fillScreen(0);
  Serial.printf("display: rotation %d -> %dx%d\n", config::kDisplayRotation,
                tft.width(), tft.height());
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}

#if PLANE_RADAR_TOUCH_UI
bool touchDown() {
  lgfx::touch_point_t tp;
  return tft.getTouchRaw(&tp, 1) > 0;
}
#endif

#if defined(BOARD_NICEMCU)
// Capacitive touch reports panel coordinates directly: nothing to calibrate.
void touchInit() {}
#elif defined(BOARD_CYD24)
namespace {
constexpr char kTouchPrefs[] = "touch";
}  // namespace

void touchInit() {
  uint16_t cal[8] = {};
  bool have = false;
  Preferences prefs;
  if (prefs.begin(kTouchPrefs, true)) {
    have = prefs.getBytesLength("cal") == sizeof(cal) &&
           prefs.getUChar("rot", 255) == config::kDisplayRotation;
    if (have) {
      prefs.getBytes("cal", cal, sizeof(cal));
    }
    prefs.end();
  }

  // Finger held on the screen for ~0.5 s at power-up = recalibrate.
  bool held = true;
  for (int i = 0; i < 10 && held; ++i) {
    held = touchDown();
    delay(50);
  }

  if (!have || held) {
    Serial.println(held ? "touch: recalibrate requested" : "touch: no calibration saved");
    tft.fillScreen(static_cast<uint16_t>(0x0000));
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setFont(&fonts::Font2);
    tft.setTextColor(config::kColorYellow, static_cast<uint16_t>(0x0000));
    tft.drawString("Touch calibration", tft.width() / 2, tft.height() / 2 - 20);
    tft.setTextColor(static_cast<uint16_t>(0xFFFF), static_cast<uint16_t>(0x0000));
    tft.drawString("Tap each corner arrow", tft.width() / 2, tft.height() / 2);
    tft.drawString(held ? "Lift your finger to start" : "as it appears",
                   tft.width() / 2, tft.height() / 2 + 20);
    while (touchDown()) {
      delay(20);
    }
    delay(300);
    tft.calibrateTouch(cal, config::kColorYellow, static_cast<uint16_t>(0x0000), 15);
    if (prefs.begin(kTouchPrefs, false)) {
      prefs.putBytes("cal", cal, sizeof(cal));
      prefs.putUChar("rot", config::kDisplayRotation);
      prefs.end();
    }
    Serial.println("touch: calibration saved");
    tft.fillScreen(static_cast<uint16_t>(0x0000));
  }
  tft.setTouchCalibrate(cal);
}
#endif
