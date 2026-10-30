#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

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
#else
constexpr gpio_num_t kBootPin = GPIO_NUM_9;  // ESP32-C3 Super Mini
#endif
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

#if defined(BOARD_CYD24)
// --- Display: ESP32-2432S024R, 2.4" ILI9341 240x320 used landscape (320x240), SPI ---
// Touch (XPT2046) shares this bus: CS 33, IRQ 36 -- not used yet.
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_NC;  // tied to EN on the CYD
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_15;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_2;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_13;
constexpr gpio_num_t kDisplayPinMiso = GPIO_NUM_12;
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_14;
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_27;

constexpr int kDisplayWidth = 320;
constexpr int kDisplayHeight = 240;
constexpr int kDisplayRotation = 1;  // landscape
/** Left edge of the 240x240 radar on the panel; the strip to its right is free. */
constexpr int kRadarOffsetX = 0;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
constexpr bool kDisplayInvert = false;
constexpr bool kDisplayRgbOrder = false;
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
#endif

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

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
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
