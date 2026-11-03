#pragma once

namespace services::travel {

/**
 * Start the background task that moves the radar's centre while the unit is on
 * the phone-hotspot Wi-Fi: live GPS from the phone (NMEA over TCP to the
 * hotspot gateway) first, an approximate Wi-Fi lookup when there is none.
 * Does nothing on the home network. Call once, after wifiSetupConnect().
 */
void start();

}  // namespace services::travel
