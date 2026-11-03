#pragma once

namespace services::net {

/**
 * One lock for everything that opens a TLS connection (the ADS-B fetch and the
 * Wi-Fi location lookup). Two handshakes at once need ~90 KB of heap, which a
 * PSRAM-less CYD does not have, so they take turns.
 */
class Guard {
 public:
  explicit Guard(unsigned long timeout_ms);
  ~Guard();
  bool ok() const { return ok_; }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  bool ok_;
};

}  // namespace services::net
