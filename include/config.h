#pragma once

#include <cstdint>

#include <driver/gpio.h>

// Boards with a touch screen share the touch UI: RANGE/MENU, tap a plane, menu.
#if defined(BOARD_CYD24) || defined(BOARD_JC3636)
#define PLANE_RADAR_TOUCH_UI 1
#else
#define PLANE_RADAR_TOUCH_UI 0
#endif

// Extra serial diagnostics and test aids (heap lines, Wi-Fi drop reasons, fetch
// failure details, the "skip home Wi-Fi" setup checkbox). OFF by default; build
// with -DPLANE_RADAR_DEBUG=1 to turn them on.
#ifndef PLANE_RADAR_DEBUG
#define PLANE_RADAR_DEBUG 0
#endif

namespace config {

// --- Wi-Fi portal ---
// Each board type gets its own setup network and network name, so two units on
// one LAN don't fight over plane-radar.local (Tim runs a CYD and a round one).
#if defined(BOARD_CYD24)
constexpr char kPortalApName[] = "PlaneRadar-CYD";
constexpr char kPortalHostname[] = "plane-radar-cyd";
constexpr char kPortalHostUrl[] = "plane-radar-cyd.local";
#elif defined(BOARD_JC3636)
constexpr char kPortalApName[] = "PlaneRadar-Round";
constexpr char kPortalHostname[] = "plane-radar-round";
constexpr char kPortalHostUrl[] = "plane-radar-round.local";
#else
constexpr char kPortalApName[] = "PlaneRadar-Setup";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";
#endif
constexpr char kPortalIp[] = "192.168.4.1";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (active LOW) ---
#if defined(BOARD_CYD24)
constexpr gpio_num_t kBootPin = GPIO_NUM_0;  // CYD BOOT button
#elif defined(BOARD_JC3636)
constexpr gpio_num_t kBootPin = GPIO_NUM_0;  // JC3636W518C BOOT button
#else
constexpr gpio_num_t kBootPin = GPIO_NUM_9;  // ESP32-C3 Super Mini
#endif
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

#if defined(BOARD_CYD24)
// --- Display: ESP32-2432S024R, 2.4" ILI9341 240x320, used PORTRAIT, SPI ---
// This panel ignores the row/column swap: rotations 1/5 still show portrait
// (Tim's bench 2026-10-30). 5 = un-mirrored portrait but software thinks 320x240;
// 4 = mirrored. So: portrait rotation 0 (try 2 if upside down).
// Touch (XPT2046) shares this bus: CS 33, IRQ 36. Used for tap = next range.
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_NC;  // tied to EN on the CYD
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_15;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_2;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_13;
constexpr gpio_num_t kDisplayPinMiso = GPIO_NUM_12;
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_14;
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_27;
constexpr gpio_num_t kTouchPinCs = GPIO_NUM_33;
constexpr gpio_num_t kTouchPinIrq = GPIO_NUM_36;
// RGB LED (common anode, active LOW) and light sensor -- usual CYD pins;
// the boot R/G/B test confirms them.
constexpr int kLedPinR = 4;
constexpr int kLedPinG = 17;  // this board: G/B swapped vs the usual CYD (Tim's bench 10-30)
constexpr int kLedPinB = 16;
constexpr int kLdrPin = 34;
// LDR calibration (raw ADC, 0 dB). Placeholders until the serial "ldr:" log
// gives real dark/bright readings. Either order works (dark may be > bright).
constexpr int kLdrDarkRaw = 300;
constexpr int kLdrBrightRaw = 0;
// LED proximity alert: airborne, closer than this and lower than this.
constexpr float kAlertDistKm = 3.219f;  // 2 mi
constexpr int kAlertAltFt = 3000;

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 320;
constexpr int kDisplayRotation = 0;  // portrait; this panel reads much clearer in rotation 0 than 2 (viewing angle, Tim 10-31)
/** Left edge of the 240x240 radar on the panel. */
constexpr int kRadarOffsetX = 0;
constexpr int kRadarOffsetY = 0;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
constexpr bool kDisplayInvert = false;
constexpr bool kDisplayRgbOrder = false;
#elif defined(BOARD_JC3636)
// --- Display: Guition JC3636W518C (_I_Y), 1.85" round 360x360, ST77916, QSPI ---
// Pins from LovyanGFX's lgfx_user/LGFX_JC3636W518.hpp (vendor pincfg.h).
// Touch: CST816 capacitive on I2C 0x15 (pins per felipedelpozo/adblock, which
// cross-checked the vendor demo) -- confirm on the bench.
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_9;
constexpr gpio_num_t kDisplayPinD0 = GPIO_NUM_11;
constexpr gpio_num_t kDisplayPinD1 = GPIO_NUM_12;
constexpr gpio_num_t kDisplayPinD2 = GPIO_NUM_13;
constexpr gpio_num_t kDisplayPinD3 = GPIO_NUM_14;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_47;
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_15;
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_7;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_8;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_40;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_41;

constexpr int kDisplayWidth = 360;
constexpr int kDisplayHeight = 360;
constexpr int kDisplayRotation = 0;
// The radar fills the panel (radar::kSize = 360 via kUiScale in radar_theme.h).
constexpr int kRadarOffsetX = 0;
constexpr int kRadarOffsetY = 0;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
constexpr bool kDisplayInvert = true;    // LovyanGFX's JC3636 config
constexpr bool kDisplayRgbOrder = true;
#else
// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;
constexpr int kDisplayRotation = 0;
constexpr int kRadarOffsetX = 0;
constexpr int kRadarOffsetY = 0;
#endif

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

// --- Travel position (radar follows a phone that provides the hotspot) ---
/** TCP port of the phone's NMEA/GPS server app (editable on the setup page). */
constexpr uint16_t kPhoneGpsDefaultPort = 10110;
/** A GPS fix older than this counts as "no GPS" (app closed, no sky view). */
constexpr unsigned long kGpsStaleMs = 30000;
/** The radar is re-centred at most this often... */
constexpr unsigned long kTravelApplyIntervalMs = 60000;
/** ...and only after moving at least this far (the first fix applies at once). */
constexpr double kTravelMinMoveKm = 1.0;
/** Wi-Fi lookup fallback: wait this long for phone GPS first, then retry this often. */
constexpr unsigned long kWifiLookupFirstDelayMs = 20000;
constexpr unsigned long kWifiLookupIntervalMs = 120000;
/** Ignore lookup answers worse than this (beaconDB falls back to IP location, 25 km). */
constexpr float kWifiLookupMaxAccuracyM = 3000.0f;
constexpr char kWifiLookupUrl[] = "https://api.beacondb.net/v1/geolocate";

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Redraw cadence; aircraft are dead-reckoned along their track/speed so motion
 *  is smooth. ~4 Hz = 250 ms (panel scanout caps at ~24 Hz; the full-frame
 *  recompose+present is the practical limit ~10-15 Hz). */
constexpr unsigned long kRadarRedrawIntervalMs = 250;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
/** CYD corner labels + menu text: the grid green, brightened to read on 2.4". */
constexpr uint16_t kColorLabelGreen = 0x3E4B;  // rgb(60, 200, 90)
/** CYD radar/menu yellow (range value, menu, selection), toned down from 0xFFE0. */
constexpr uint16_t kColorUiYellow = 0xDE40;  // rgb(220, 200, 0)
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
