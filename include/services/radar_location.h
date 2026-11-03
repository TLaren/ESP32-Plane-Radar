#pragma once

namespace services::location {

/** Where the radar's current position came from. */
enum class Source : unsigned char {
  Saved,       // the home position saved on the setup page
  PhoneGps,    // live GPS from a phone, over its hotspot
  WifiLookup,  // approximate, from nearby Wi-Fi networks
};

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/**
 * The position the radar is centred on: a travel position (phone GPS or Wi-Fi
 * lookup) while one is active, otherwise the saved home position. Safe to call
 * from any task.
 */
double lat();
double lon();

/** The saved home position (what the setup page shows and edits). */
double homeLat();
double homeLon();

Source source();
const char* sourceLabel();

/** Use a travel position (RAM only; the saved home position is untouched). */
void setTravel(double lat, double lon, Source source);
/** Back to the saved home position (e.g. on the home Wi-Fi again). */
void clearTravel();

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
