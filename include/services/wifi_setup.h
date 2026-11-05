#pragma once

#include <cstdint>

#include "config.h"

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
#if PLANE_RADAR_TOUCH_UI
/** Full-screen "Reset Wi-Fi?" prompt; true if the screen is tapped within 5 s. */
bool confirmWifiResetByTouch();
#endif
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** True while connected to the saved phone-hotspot network (not the home one). */
bool wifiOnPhoneNetwork();
/** TCP port of the phone's GPS app, as saved on the setup page. */
uint16_t phoneGpsPort();
/** Travel mode switch from the setup page. Default: on with PSRAM, off without. */
bool travelModeEnabled();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
