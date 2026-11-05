#include "services/adsb_client.h"
#include "services/net_lock.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;  // TLS handshake needs room
constexpr unsigned long kRequestTimeoutMs = 6000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectTimeoutMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback, which is why HTTPClient's own
 * body readers (getString/writeToStream) can't be used here -- they block
 * without giving the fetch task a chance to poll. That also means they can't
 * de-chunk for us, so the wire image comes out raw and BodyFramer unwraps it.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(HTTPClient& http, WiFiClient& stream,
                      unsigned long deadline)
      : http_(&http), stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!http_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  HTTPClient* http_;
  WiFiClient* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

#ifndef ADSB_WHOLE_LIST_PARSE
constexpr size_t kPlaneBufLen = 1536;

/**
 * Streams the response and parses ONE aircraft object at a time.
 *
 * The whole-list parse keeps every aircraft in RAM while the TLS session is
 * still holding its buffers, so a busy sky (about 14+ planes on a CYD, which
 * has no PSRAM) ran out of memory mid-parse. Here each {...} inside "ac" is
 * collected into a small buffer, parsed into a small reused document, copied
 * into `out`, and dropped, so peak memory no longer depends on how many
 * aircraft are in the sky.
 *
 * Returns false with *why set if the stream ended or broke before "ac" closed.
 */
template <typename Reader>
bool parseAircraftStream(Reader& body, Aircraft* out, size_t* count,
                         const char** why, char* buf, size_t buf_len,
                         const JsonDocument& filter) {
  // buf (one aircraft object, raw JSON) and filter are made by the caller
  // BEFORE the TLS connection opens, while there is still heap to spare.
  JsonDocument one;

  int depth = 0;
  bool in_str = false;
  bool esc = false;
  bool in_ac = false;
  bool collecting = false;
  bool overflow = false;
  size_t len = 0;
  char last_str[4] = {0, 0, 0, 0};  // last top-level string: spots the "ac" key
  size_t last_len = 0;
  bool finished = false;
  size_t n = 0;
  size_t dropped = 0;

  int c;
  while ((c = body.read()) >= 0) {
    const char ch = static_cast<char>(c);
    if (collecting) {
      if (len < buf_len - 1) {
        buf[len++] = ch;
      } else {
        overflow = true;
      }
    }
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (ch == '\\') {
        esc = true;
      } else if (ch == '"') {
        in_str = false;
      } else if (depth == 1 && last_len < sizeof(last_str)) {
        last_str[last_len++] = ch;
      }
      continue;
    }
    switch (ch) {
      case '"':
        in_str = true;
        if (depth == 1) {
          last_len = 0;
          memset(last_str, 0, sizeof(last_str));
        }
        break;
      case '{':
      case '[':
        ++depth;
        if (ch == '[' && depth == 2 && last_str[0] == 'a' && last_str[1] == 'c' &&
            last_str[2] == '\0') {
          in_ac = true;
        } else if (ch == '{' && in_ac && depth == 3) {
          collecting = true;
          overflow = false;
          len = 0;
          buf[len++] = '{';
        }
        break;
      case '}':
      case ']':
        if (ch == '}' && collecting && depth == 3) {
          collecting = false;
          if (overflow) {
            ++dropped;
          } else if (n < kMaxAircraft) {
            one.clear();
            const DeserializationError e = deserializeJson(
                one, buf, len, DeserializationOption::Filter(filter));
            const JsonObject plane = one.as<JsonObject>();
            if (!e && !plane.isNull() && plane["lat"].is<float>() &&
                plane["lon"].is<float>() &&
                (config::kAdsbShowGroundAircraft || !isOnGround(plane))) {
              Aircraft& a = out[n];
              a.lat = plane["lat"].as<float>();
              a.lon = plane["lon"].as<float>();
              a.nose_deg = pickNoseHeading(plane);
              a.track_deg = pickTrackHeading(plane);
              a.gs_knots = pickGroundSpeed(plane);
              float seen_pos = 0.0f;
              readJsonFloat(plane, "seen_pos", &seen_pos);
              if (seen_pos < 0.0f) seen_pos = 0.0f;
              if (seen_pos > 30.0f) seen_pos = 30.0f;
              a.pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);
              fillTagFields(&a, plane);
              ++n;
            }
          }
        }
        --depth;
        if (ch == ']' && in_ac && depth == 1) {
          finished = true;
        }
        break;
      default:
        break;
    }
    if (finished) {
      break;
    }
  }

  *count = n;
  if (dropped > 0) {
    Serial.printf("adsb: %u oversized aircraft skipped\n",
                  static_cast<unsigned>(dropped));
  }
  if (!finished) {
    *why = "IncompleteInput";
    return false;
  }
  return true;
}
#endif  // ADSB_WHOLE_LIST_PARSE

}  // namespace

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  // Take turns with the Wi-Fi location lookup (see net_lock.h).
  services::net::Guard net_guard(30000);
  if (!net_guard.ok()) {
    return false;
  }
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

#ifdef ADSB_WHOLE_LIST_PARSE
  // Keep only the fields we render; the rest never reaches RAM.
  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category"}) {
    f[key] = true;
  }

#endif

#ifndef ADSB_WHOLE_LIST_PARSE
  // On this task's stack (16 KB, reserved at start), NOT the heap: the TLS
  // handshake needs nearly all of the heap on a board without PSRAM.
  char plane_buf[kPlaneBufLen];
  JsonDocument plane_filter;  // object-level: the fields we render
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category"}) {
    plane_filter[key] = true;
  }
#endif

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  // HTTPClient only records Transfer-Encoding in the collected headers when
  // it is asked for up front; _transferEncoding itself is private.
  static const char* kWantedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kWantedHeaders, 1);

  http.setTimeout(kRequestTimeoutMs);
#if PLANE_RADAR_DEBUG
  const unsigned long fetch_t0 = millis();
#endif
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
#if PLANE_RADAR_DEBUG
    Serial.printf("adsb: HTTP %d (free %u, blk %u, min %u)\n", code,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
#else
    Serial.printf("adsb: HTTP %d\n", code);
#endif
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    return false;
  }

  // On HTTP/1.1 the CDN answers with Transfer-Encoding: chunked, and
  // getStreamPtr() hands back the raw socket -- chunk sizes and all. BodyFramer
  // strips that framing back off.
  const services::http::BodyFraming framing =
      http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
          ? services::http::BodyFraming::kChunked
          : services::http::BodyFraming::kIdentity;

  PollingSocketSource source(http, *stream, millis() + kRequestTimeoutMs);
  BodyReader body(source, framing, http.getSize());
#ifndef ADSB_WHOLE_LIST_PARSE
  Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  const char* why = "";
  const bool ok = parseAircraftStream(body, parsed, &n, &why, plane_buf,
                                      kPlaneBufLen, plane_filter);
  body.drain();
  http.end();
  if (!ok) {
    if (body.framingError()) {
      Serial.println("adsb: malformed chunked body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
#if PLANE_RADAR_DEBUG
      Serial.printf("adsb: JSON parse error: %s (free %u, blk %u, min %u) "
                    "[wifi %s, rssi %d, got %u bytes in %lu ms, stack free %u]\n",
                    why, static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(ESP.getMinFreeHeap()),
                    WiFi.status() == WL_CONNECTED ? "up" : "DOWN", WiFi.RSSI(),
                    static_cast<unsigned>(body.bytesRead()), millis() - fetch_t0,
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#else
      Serial.printf("adsb: JSON parse error: %s\n", why);
#endif
    }
    return false;
  }
  // A good response with an empty list while planes were showing is almost
  // always a feed hiccup: keep the last list instead of blanking the screen.
  // Three zeros in a row are believed (the sky really is empty).
  static uint8_t s_zero_streak = 0;
  if (n == 0 && s_aircraft_count >= 3 && s_zero_streak < 2) {
    ++s_zero_streak;
    Serial.printf("adsb: 0 aircraft ignored (keeping %u)\n",
                  static_cast<unsigned>(s_aircraft_count));
    return false;
  }
  s_zero_streak = 0;
  publish(parsed, n);
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}
#else
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  // Read off the terminating chunk the parser stopped short of, so the socket
  // sits at the end of the message. Not needed while every fetch builds its own
  // connection, but a prerequisite for ever reusing one.
  body.drain();
  http.end();
  if (err) {
    if (body.framingError()) {
      Serial.println("adsb: malformed chunked body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
      Serial.printf("adsb: JSON parse error: %s (free %u, blk %u, min %u)\n", err.c_str(),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(ESP.getMinFreeHeap()));
    }
    return false;
  }

  // Parse into a local buffer, then publish atomically so a reader on another
  // thread never sees a half-updated list.
  Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (!ac.isNull()) {
    for (JsonObject plane : ac) {
      if (n >= kMaxAircraft) {
        break;
      }
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
        continue;
      }
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
        continue;
      }

      parsed[n].lat = plane["lat"].as<float>();
      parsed[n].lon = plane["lon"].as<float>();
      parsed[n].nose_deg = pickNoseHeading(plane);
      parsed[n].track_deg = pickTrackHeading(plane);
      parsed[n].gs_knots = pickGroundSpeed(plane);

      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      parsed[n].pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);

      fillTagFields(&parsed[n], plane);
      ++n;
    }
  }

  publish(parsed, n);
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}
#endif

}  // namespace services::adsb
