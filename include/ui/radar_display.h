#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

#if defined(BOARD_CYD24)
/** Touch: select the plane drawn nearest screen (x, y), within ~24 px. Shown in
 *  the info panel + ringed for 30 s. Tapping empty radar clears; returns false. */
bool radarSelectAt(int x, int y);
void radarClearSelection();
#endif

}  // namespace ui
