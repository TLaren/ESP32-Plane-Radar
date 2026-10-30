#pragma once

#include "hardware/lgfx_config.hpp"

extern LGFX tft;

void displayInit();

#if defined(BOARD_CYD24)
/** Load the saved touch calibration, or run the 4-corner calibration if there
 *  is none (or a finger is on the screen at power-up). Call after displayInit. */
void touchInit();
/** True while the screen is being pressed (no calibration needed). */
bool touchDown();
#endif
