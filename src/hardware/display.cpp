#include "hardware/display.h"

#include "config.h"

#include "hardware/display_font.h"

LGFX tft;

void displayInit() {
  tft.init();
  tft.setRotation(config::kDisplayRotation);
  tft.fillScreen(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}
