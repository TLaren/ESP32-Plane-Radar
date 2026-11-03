#pragma once

#include <cmath>

namespace services::geo {

/** Great-circle distance in km (haversine). Pure maths, testable on a PC. */
inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double kEarthKm = 6371.0088;
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  const double dlat = (lat2 - lat1) * kDeg;
  const double dlon = (lon2 - lon1) * kDeg;
  const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(lat1 * kDeg) * std::cos(lat2 * kDeg) *
                       std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2.0 * kEarthKm * std::asin(std::sqrt(a));
}

}  // namespace services::geo
