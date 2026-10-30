#pragma once

#if defined(BOARD_CYD24)

namespace ui::menu {

bool isOpen();
/** Draw the settings page full screen (radar redraw pauses while open). */
void open();
/** Handle a tap while open. Returns false once the menu has closed. */
bool tap(int x, int y);

}  // namespace ui::menu

#endif
