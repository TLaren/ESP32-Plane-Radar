#include "services/nmea.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace services::nmea {

namespace {

constexpr size_t kMaxFields = 16;
constexpr size_t kMaxLen = 120;  // NMEA allows 82; some apps add a little

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/** Splits `s` in place at commas (empty fields kept); returns the field count. */
size_t split(char* s, const char** fields, size_t max_fields) {
  size_t n = 0;
  fields[n++] = s;
  for (char* p = s; *p != '\0'; ++p) {
    if (*p == ',') {
      *p = '\0';
      if (n < max_fields) {
        fields[n++] = p + 1;
      }
    }
  }
  return n;
}

bool parseNumber(const char* s, double* out) {
  if (s == nullptr || s[0] == '\0') return false;
  char* end = nullptr;
  const double v = strtod(s, &end);
  if (end == s || *end != '\0') return false;
  *out = v;
  return true;
}

/** "ddmm.mmmm" / "dddmm.mmmm" plus a hemisphere letter -> signed degrees. */
bool parseCoord(const char* value, const char* hemi, double* out) {
  double raw = 0.0;
  if (!parseNumber(value, &raw) || raw < 0.0) return false;
  if (hemi == nullptr || hemi[0] == '\0' || hemi[1] != '\0') return false;
  const int deg = static_cast<int>(raw / 100.0);
  const double minutes = raw - deg * 100.0;
  if (minutes >= 60.0) return false;
  double d = deg + minutes / 60.0;
  switch (hemi[0]) {
    case 'N':
    case 'E':
      break;
    case 'S':
    case 'W':
      d = -d;
      break;
    default:
      return false;
  }
  *out = d;
  return true;
}

}  // namespace

bool parseSentence(const char* line, Fix* out) {
  if (line == nullptr || out == nullptr || line[0] != '$') return false;
  const size_t len = strlen(line);
  if (len < 7 || len >= kMaxLen) return false;

  char buf[kMaxLen];
  memcpy(buf, line, len + 1);

  char* star = strchr(buf, '*');
  if (star != nullptr) {
    if (strlen(star) < 3) return false;
    const int hi = hexVal(star[1]);
    const int lo = hexVal(star[2]);
    if (hi < 0 || lo < 0) return false;
    unsigned sum = 0;
    for (const char* p = buf + 1; p < star; ++p) {
      sum ^= static_cast<unsigned char>(*p);
    }
    if (sum != static_cast<unsigned>(hi * 16 + lo)) return false;
    *star = '\0';
  }

  const char* f[kMaxFields];
  const size_t n = split(buf, f, kMaxFields);
  if (strlen(f[0]) != 6) return false;  // "$" + 2 talker + 3 type
  const char* type = f[0] + 3;

  double lat = 0.0;
  double lon = 0.0;
  float speed = -1.0f;

  if (strcmp(type, "GGA") == 0) {
    if (n < 7) return false;
    if (atoi(f[6]) < 1) return false;  // 0 = no fix
    if (!parseCoord(f[2], f[3], &lat) || !parseCoord(f[4], f[5], &lon)) {
      return false;
    }
  } else if (strcmp(type, "RMC") == 0) {
    if (n < 8) return false;
    if (strcmp(f[2], "A") != 0) return false;  // V = void
    if (!parseCoord(f[3], f[4], &lat) || !parseCoord(f[5], f[6], &lon)) {
      return false;
    }
    double kn = 0.0;
    if (parseNumber(f[7], &kn) && kn >= 0.0) speed = static_cast<float>(kn);
  } else {
    return false;
  }

  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
  out->lat = lat;
  out->lon = lon;
  out->speed_kn = speed;
  return true;
}

}  // namespace services::nmea
