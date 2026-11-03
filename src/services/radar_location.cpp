#include "services/radar_location.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";

// Home = saved on the setup page. Travel = phone GPS / Wi-Fi lookup, RAM only.
// The travel values are written by the travel task and read by the ADS-B task
// and the UI, so every access is under this spinlock (a double is two words
// on the ESP32 and could otherwise be read half-written).
double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
double s_travel_lat = 0.0;
double s_travel_lon = 0.0;
Source s_source = Source::Saved;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void persist(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.end();
  portENTER_CRITICAL(&s_mux);
  s_lat = lat;
  s_lon = lon;
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  prefs.end();
}

double lat() {
  portENTER_CRITICAL(&s_mux);
  const double v = (s_source == Source::Saved) ? s_lat : s_travel_lat;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

double lon() {
  portENTER_CRITICAL(&s_mux);
  const double v = (s_source == Source::Saved) ? s_lon : s_travel_lon;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

double homeLat() {
  portENTER_CRITICAL(&s_mux);
  const double v = s_lat;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

double homeLon() {
  portENTER_CRITICAL(&s_mux);
  const double v = s_lon;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

Source source() {
  portENTER_CRITICAL(&s_mux);
  const Source v = s_source;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

const char* sourceLabel() {
  switch (source()) {
    case Source::PhoneGps:
      return "phone GPS";
    case Source::WifiLookup:
      return "Wi-Fi lookup";
    default:
      return "saved";
  }
}

void setTravel(double lat, double lon, Source src) {
  if (!validLatLon(lat, lon) || src == Source::Saved) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  s_travel_lat = lat;
  s_travel_lon = lon;
  s_source = src;
  portEXIT_CRITICAL(&s_mux);
}

void clearTravel() {
  portENTER_CRITICAL(&s_mux);
  s_source = Source::Saved;
  portEXIT_CRITICAL(&s_mux);
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persist(lat, lon);
  Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.end();
  portENTER_CRITICAL(&s_mux);
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace services::location
