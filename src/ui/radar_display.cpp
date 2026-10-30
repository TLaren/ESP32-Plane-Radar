#include "ui/radar_display.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

/**
 * Dead-reckon an aircraft's position from its last-fetched fix along its ground
 * track, so it moves smoothly between ADS-B updates. Uses the same flat
 * 1° ≈ 111 km projection as offsetKmFromCenter(), so it round-trips exactly.
 */
void extrapolatedLatLon(const services::adsb::Aircraft& plane,
                        unsigned long base_ms, float* lat, float* lon) {
  *lat = plane.lat;
  *lon = plane.lon;
  if (base_ms == 0 || plane.gs_knots <= 0.0f) {
    return;
  }
  // Elapsed since the fix was measured = time since fetch + the fix's own age.
  const unsigned long elapsed_ms = (millis() - base_ms) + plane.pos_age_ms;
  const float elapsed_h = static_cast<float>(elapsed_ms) / 3600000.0f;
  const float dist_km = plane.gs_knots * 1.852f * elapsed_h;  // knots -> km
  if (dist_km <= 0.0f) {
    return;
  }
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = plane.track_deg * kDegToRad;  // track: 0 = N, 90 = E
  *lat = plane.lat + (dist_km * cosf(rad)) / kKmPerDeg;
  *lon = plane.lon + (dist_km * sinf(rad)) / kKmPerDeg;
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  // West (left): tag toward center on the right; east (right): tag on the left.
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagAltitude, radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  // Snapshot under the adsb lock (the fetch may run on another thread).
  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      planes, services::adsb::kMaxAircraft, &base_ms);

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    // Dead-reckoned position for smooth motion between fetches.
    float lat = 0.0f;
    float lon = 0.0f;
    extrapolatedLatLon(planes[i], base_ms, &lat, &lon);

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(lat, lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(lat, lon, &dot_x, &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    drawAircraftTag(items[d].x, items[d].y, planes[i]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
#if !defined(BOARD_CYD24)
  drawScaleLabel(cx, cy, grid_r);
#endif
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

#if defined(BOARD_CYD24)
// CYD 2.4" (240x320 glass): info panel in the area the 240x240 radar disc
// doesn't use. Geometry comes from the live tft size, so it works either way:
//   landscape 320x240 -> 80x240 strip on the right
//   portrait  240x320 -> 240x80 strip along the bottom
// Count = everything inside the outer ring; NEAREST/NEXT = airborne only, so
// aircraft parked at a nearby airport don't sit at the top. Redrawn at 1 Hz.
// A plane tapped on the radar replaces NEAREST (yellow box + yellow ring on
// the radar) for kSelectHoldMs, or until it leaves the radar.
LGFX_Sprite s_panel(&tft);
bool s_panel_ready = false;
bool s_panel_failed = false;
unsigned long s_panel_last_ms = 0;

constexpr unsigned long kSelectHoldMs = 30000;
constexpr int kSelectTapRadiusPx = 24;  // resistive touch is only roughly accurate
char s_sel_key[sizeof(services::adsb::Aircraft::callsign)] = "";
unsigned long s_sel_until_ms = 0;
services::adsb::Aircraft s_sel_planes[services::adsb::kMaxAircraft];

struct PanelItem {
  const services::adsb::Aircraft* plane;
  float dist_km;
  float brg_deg;
};

void clearSelection() {
  if (s_sel_key[0] != '\0') {
    Serial.println("select: cleared");
  }
  s_sel_key[0] = '\0';
  s_panel_last_ms = 0;  // repaint the panel on the next frame
}

bool selectionActive() {
  if (s_sel_key[0] == '\0') {
    return false;
  }
  if (static_cast<long>(s_sel_until_ms - millis()) <= 0) {
    clearSelection();
    return false;
  }
  return true;
}

/** Dead-reckoned range/bearing/screen position; false if outside the radar disc. */
bool placePlane(const services::adsb::Aircraft& plane, unsigned long base_ms,
                PanelItem* item, int* x, int* y) {
  float lat = 0.0f;
  float lon = 0.0f;
  extrapolatedLatLon(plane, base_ms, &lat, &lon);
  float dx = 0.0f;
  float dy = 0.0f;
  float dist = 0.0f;
  offsetKmFromCenter(lat, lon, &dx, &dy, &dist);
  if (!isInsideOuterRingKm(dist)) {
    return false;
  }
  float brg = atan2f(dx, dy) / kDegToRad;
  if (brg < 0.0f) brg += 360.0f;
  *item = {&plane, dist, brg};
  latLonToScreen(lat, lon, x, y);
  return true;
}

/** Find the selected plane in a snapshot; clears the selection if it's gone. */
bool findSelected(const services::adsb::Aircraft* planes, size_t n,
                  unsigned long base_ms, PanelItem* item, int* x, int* y) {
  if (!selectionActive()) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (strcmp(planes[i].callsign, s_sel_key) == 0) {
      if (placePlane(planes[i], base_ms, item, x, y)) {
        return true;
      }
      break;
    }
  }
  clearSelection();
  return false;
}

void drawSelectionRing() {
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      s_sel_planes, services::adsb::kMaxAircraft, &base_ms);
  PanelItem it{};
  int x = 0;
  int y = 0;
  if (!findSelected(s_sel_planes, n, base_ms, &it, &x, &y)) {
    return;
  }
  s_draw->drawCircle(x, y, 13, config::kColorUiYellow);
  s_draw->drawCircle(x, y, 14, config::kColorUiYellow);
}

const char* compass8(float deg) {
  static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kPoints[static_cast<int>(lroundf(deg / 45.0f)) & 7];
}

void formatPanelDist(char* buf, size_t len, const PanelItem& it) {
  const float d = radar::useMiles() ? it.dist_km * 0.621371f : it.dist_km;
  snprintf(buf, len, "%.1f%s %s", d, radar::useMiles() ? "mi" : "km",
           compass8(it.brg_deg));
}

const char* panelName(const services::adsb::Aircraft& p) {
  if (p.callsign[0] != '\0') return p.callsign;
  if (p.type[0] != '\0') return p.type;
  return "------";
}

void panelText(const char* text, int x, int y, uint16_t color) {
  s_panel.setTextColor(color);
  s_panel.drawString(text, x, y);
}

// `first` = selected plane (selected=true) or the nearest airborne; may be null.
// `next` = up to two more, nearest first.
// Right-hand strip (landscape), 80 px wide.
void drawPanelSide(size_t on_screen, const PanelItem* first, bool selected,
                   const PanelItem* next, size_t next_count) {
  const int w = s_panel.width();
  const int cx = w / 2;
  char buf[24];
  s_panel.drawFastVLine(0, 0, s_panel.height(), radar::kColorGrid);
  s_panel.setTextDatum(textdatum_t::top_center);
  s_panel.setFont(&fonts::Font2);
  panelText("PLANES", cx, 4, radar::kColorGrid);
  s_panel.setFont(&fonts::Font4);
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(on_screen));
  panelText(buf, cx, 22, radar::kColorLabel);
  s_panel.drawFastHLine(4, 52, w - 8, radar::kColorGrid);
  s_panel.setFont(&fonts::Font2);
  panelText(selected ? "SELECTED" : "NEAREST", cx, 56,
            selected ? config::kColorUiYellow : radar::kColorGrid);
  if (first == nullptr) {
    panelText("none", cx, 74, radar::kColorLabel);
  } else {
    const auto& p = *first->plane;
    panelText(panelName(p), cx, 74, radar::kColorLabel);
    panelText(p.type, cx, 90, radar::kColorTagType);
    panelText(p.alt, cx, 106, radar::kColorTagAltitude);
    formatPanelDist(buf, sizeof(buf), *first);
    panelText(buf, cx, 122, radar::kColorLabel);
  }
  s_panel.drawFastHLine(4, 142, w - 8, radar::kColorGrid);
  panelText("NEXT", cx, 146, radar::kColorGrid);
  for (size_t i = 0; i < next_count; ++i) {
    const int y = 164 + static_cast<int>(i) * 34;
    panelText(panelName(*next[i].plane), cx, y, radar::kColorLabel);
    formatPanelDist(buf, sizeof(buf), next[i]);
    panelText(buf, cx, y + 16, radar::kColorTagAltitude);
  }
}

// Bottom strip (portrait), 240x80: | count | nearest/selected | next two |
void drawPanelBottom(size_t on_screen, const PanelItem* first, bool selected,
                     const PanelItem* next, size_t next_count) {
  constexpr int kColCount = 52;
  constexpr int kColNearest = 92;
  const int x1 = kColCount;
  const int x2 = kColCount + kColNearest;
  char buf[24];
  s_panel.drawFastHLine(0, 0, s_panel.width(), radar::kColorGrid);
  s_panel.drawFastVLine(x1, 4, s_panel.height() - 8, radar::kColorGrid);
  s_panel.drawFastVLine(x2, 4, s_panel.height() - 8, radar::kColorGrid);
  if (selected) {
    s_panel.drawRect(x1 + 1, 1, kColNearest - 1, s_panel.height() - 1,
                     config::kColorUiYellow);
  }

  s_panel.setTextDatum(textdatum_t::top_center);
  s_panel.setFont(&fonts::Font2);
  panelText("PLANES", kColCount / 2, 6, radar::kColorGrid);
  s_panel.setFont(&fonts::Font4);
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(on_screen));
  panelText(buf, kColCount / 2, 30, radar::kColorLabel);

  s_panel.setFont(&fonts::Font2);
  s_panel.setTextDatum(textdatum_t::top_left);
  const int nx = x1 + 5;
  if (first == nullptr) {
    panelText("NEAREST", nx, 2, radar::kColorGrid);
    panelText("none", nx, 18, radar::kColorLabel);
  } else {
    const auto& p = *first->plane;
    panelText(panelName(p), nx, 2, selected ? config::kColorUiYellow : radar::kColorLabel);
    panelText(p.type, nx, 18, radar::kColorTagType);
    panelText(p.alt, nx, 34, radar::kColorTagAltitude);
    formatPanelDist(buf, sizeof(buf), *first);
    panelText(buf, nx, 50, radar::kColorLabel);
  }

  const int xx = x2 + 5;
  panelText("NEXT", xx, 2, radar::kColorGrid);
  for (size_t i = 0; i < next_count; ++i) {
    const int y = 18 + static_cast<int>(i) * 32;
    panelText(panelName(*next[i].plane), xx, y, radar::kColorLabel);
    formatPanelDist(buf, sizeof(buf), next[i]);
    panelText(buf, xx, y + 16, radar::kColorTagAltitude);
  }
}

void drawSidePanel() {
  if (s_panel_failed ||
      (s_panel_last_ms != 0 && millis() - s_panel_last_ms < 1000)) {
    return;
  }
  s_panel_last_ms = millis();
  const bool side = tft.width() > radar::kSize;
  const int pw = side ? tft.width() - radar::kSize : tft.width();
  const int ph = side ? tft.height() : tft.height() - radar::kSize;
  if (pw <= 0 || ph <= 0) {
    return;
  }
  if (!s_panel_ready) {
    s_panel.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
    if (!s_panel.createSprite(pw, ph)) {
      Serial.println("radar: info panel sprite alloc failed");
      s_panel_failed = true;
      return;
    }
    Serial.printf("radar: info panel %dx%d (%s)\n", pw, ph, side ? "right" : "bottom");
    s_panel_ready = true;
  }

  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      planes, services::adsb::kMaxAircraft, &base_ms);

  PanelItem items[services::adsb::kMaxAircraft];
  size_t on_screen = 0;
  size_t airborne = 0;
  for (size_t i = 0; i < n; ++i) {
    PanelItem it{};
    int x = 0;
    int y = 0;
    if (!placePlane(planes[i], base_ms, &it, &x, &y)) continue;
    ++on_screen;
    if (strcmp(planes[i].alt, "GND") == 0) continue;
    items[airborne++] = it;
  }
  std::sort(items, items + airborne, [](const PanelItem& a, const PanelItem& b) {
    return a.dist_km < b.dist_km;
  });

  PanelItem sel{};
  int sx = 0;
  int sy = 0;
  const bool selected = findSelected(planes, n, base_ms, &sel, &sx, &sy);
  const PanelItem* first = selected ? &sel : (airborne > 0 ? &items[0] : nullptr);
  PanelItem next[2];
  size_t next_count = 0;
  for (size_t i = 0; i < airborne && next_count < 2; ++i) {
    if (items[i].plane == first->plane) continue;
    next[next_count++] = items[i];
  }

  s_panel.fillScreen(radar::kColorBackground);
  if (side) {
    drawPanelSide(on_screen, first, selected, next, next_count);
  } else {
    drawPanelBottom(on_screen, first, selected, next, next_count);
  }
  s_panel.pushSprite(side ? config::kRadarOffsetX + radar::kSize : 0,
                     side ? 0 : radar::kSize);
}
#endif  // BOARD_CYD24

#if defined(BOARD_CYD24)
// CYD: the range moves off ring 3 (faint green, buried under tags when busy)
// into the free top-left corner, drawn AFTER the aircraft so nothing covers it.
// The value is still the ring-3 distance (rings are 1/3 of it apart).
void drawCornerRange() {
  char label[12];
  radar::formatCurrentRing3Label(label, sizeof(label));
  s_draw->setTextSize(1);
  s_draw->setTextDatum(textdatum_t::top_left);
  s_draw->setFont(&fonts::Font2);
  s_draw->setTextColor(config::kColorLabelGreen, radar::kColorBackground);
  s_draw->drawString("RANGE", 3, 2);
  s_draw->setFont(&fonts::FreeSansBold9pt7b);
  s_draw->setTextColor(config::kColorUiYellow, radar::kColorBackground);
  s_draw->drawString(label, 3, 19);

  // Top-right: MENU (opens settings), mirrors RANGE on the left.
  const int rx = radar::kSize - 3;
  s_draw->setFont(&fonts::Font2);
  s_draw->setTextDatum(textdatum_t::top_right);
  s_draw->setTextColor(config::kColorLabelGreen, radar::kColorBackground);
  s_draw->drawString("MENU", rx, 2);
  for (int i = 0; i < 3; ++i) {
    s_draw->fillRect(rx - 22, 21 + i * 6, 22, 3, config::kColorUiYellow);
  }
  s_draw->setTextDatum(textdatum_t::top_left);
}
#endif

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft();
#if defined(BOARD_CYD24)
    drawSelectionRing();
    drawCornerRange();
#endif
  }
  s_frame.pushSprite(config::kRadarOffsetX, 0);
#if defined(BOARD_CYD24)
  drawSidePanel();
#endif
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
#if defined(BOARD_CYD24)
  s_panel_last_ms = 0;  // full redraw: repaint the info strip right away too
#endif
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  tft.setTextDatum(textdatum_t::top_left);
}

#if defined(BOARD_CYD24)
bool radarSelectAt(int tx, int ty) {
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      s_sel_planes, services::adsb::kMaxAircraft, &base_ms);
  int best = kSelectTapRadiusPx * kSelectTapRadiusPx + 1;
  const services::adsb::Aircraft* pick = nullptr;
  for (size_t i = 0; i < n; ++i) {
    PanelItem it{};
    int x = 0;
    int y = 0;
    if (!placePlane(s_sel_planes[i], base_ms, &it, &x, &y)) continue;
    const int d = (x - tx) * (x - tx) + (y - ty) * (y - ty);
    if (d < best) {
      best = d;
      pick = &s_sel_planes[i];
    }
  }
  if (pick == nullptr || pick->callsign[0] == '\0') {
    clearSelection();
    return false;
  }
  strncpy(s_sel_key, pick->callsign, sizeof(s_sel_key) - 1);
  s_sel_key[sizeof(s_sel_key) - 1] = '\0';
  s_sel_until_ms = millis() + kSelectHoldMs;
  s_panel_last_ms = 0;
  Serial.printf("select: %s (%d px from tap)\n", s_sel_key,
                static_cast<int>(lroundf(sqrtf(static_cast<float>(best)))));
  return true;
}

void radarClearSelection() { clearSelection(); }
#endif

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

}  // namespace ui
