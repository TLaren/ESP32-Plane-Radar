#pragma once

namespace services::travel {

/**
 * Call every loop(). The first time the unit is on the phone-hotspot Wi-Fi it
 * starts the background task that moves the radar's centre: live GPS from the
 * phone (NMEA over TCP to the hotspot gateway) first, an approximate Wi-Fi
 * lookup when there is none. At home it does nothing and costs no memory.
 */
void poll();

}  // namespace services::travel
