#pragma once

#include "config.h"

#if PLANE_RADAR_TOUCH_UI

namespace cyd {

/** Brightness settings (both touch boards); RGB LED + light sensor on the CYD
 *  only (the JC3636 has neither, so its LED/auto settings are inert). */
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
