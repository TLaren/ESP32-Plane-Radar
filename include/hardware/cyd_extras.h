#pragma once

#if defined(BOARD_CYD24)

namespace cyd {

/** RGB LED, light sensor and the brightness settings. Call after touchInit(). */
void init();
/** LED state machine + auto brightness. Call every loop(). */
void loop();

int screenPct();
int ledPct();
bool autoBrightness();
void setScreenPct(int pct);
void setLedPct(int pct);
void setAutoBrightness(bool on);
/** Persist the three settings to flash (called when the menu closes). */
void saveSettings();

}  // namespace cyd

#endif
