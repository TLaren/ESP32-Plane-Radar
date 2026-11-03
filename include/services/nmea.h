#pragma once

namespace services::nmea {

struct Fix {
  double lat;       // signed degrees, + = north
  double lon;       // signed degrees, + = east
  float speed_kn;   // RMC ground speed in knots; < 0 when the sentence has none
};

/**
 * Parse one NMEA 0183 sentence (no trailing CR/LF). Returns true only for a
 * GGA or RMC sentence (any talker: GP, GN, GL, GA, ...) that carries a valid
 * position: GGA quality >= 1, or RMC status 'A'. A checksum, when present,
 * must match; a sentence without one is accepted (TCP is already reliable).
 * Pure C++ (no Arduino), so it can be built and tested on a PC.
 */
bool parseSentence(const char* line, Fix* out);

}  // namespace services::nmea
