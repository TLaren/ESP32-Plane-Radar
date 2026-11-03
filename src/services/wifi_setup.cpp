#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#if PLANE_RADAR_TOUCH_UI
#include "hardware/display.h"
#endif
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

// Second network for trips: the phone's hotspot. Stored in our own NVS
// namespace; the home network stays in the ESP-IDF flash config (WiFiManager).
constexpr char kPhonePrefsNamespace[] = "wifi2";
constexpr char kPhoneKeySsid[] = "ssid";
constexpr char kPhoneKeyPass[] = "pass";
constexpr char kPhoneKeyPort[] = "port";

WiFiManagerParameter s_param_phone_ssid(
    "phone_ssid", "Phone hotspot name (2nd network, optional)", "", 32);
WiFiManagerParameter s_param_phone_pass(
    "phone_pass", "Phone hotspot password (leave blank to keep)", "", 63,
    " type=\"password\"");
WiFiManagerParameter s_param_gps_port("gps_port", "Phone GPS app port", "10110", 5,
                                      " type=\"number\" min=\"1\" max=\"65535\"");

char s_phone_ssid[33] = "";  // RAM copy so other tasks need no NVS access

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

void refreshPortalParamValues() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  // Always the saved HOME position: while travelling lat()/lon() hold the phone's
  // position, and saving that back as "home" would be wrong.
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::homeLat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::homeLon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);

  Preferences phone;
  if (phone.begin(kPhonePrefsNamespace, false)) {
    const String ssid = phone.getString(kPhoneKeySsid, "");
    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u",
             static_cast<unsigned>(phone.getUShort(kPhoneKeyPort, config::kPhoneGpsDefaultPort)));
    phone.end();
    s_param_phone_ssid.setValue(ssid.c_str(), 32);
    s_param_gps_port.setValue(port_buf, 5);
  }
  s_param_phone_pass.setValue("", 63);  // never echo a stored password
}

void savePhoneNetworkFromPortal() {
  String ssid = s_param_phone_ssid.getValue();
  ssid.trim();
  const String pass = s_param_phone_pass.getValue();
  long port = atol(s_param_gps_port.getValue());
  if (port < 1 || port > 65535) {
    port = config::kPhoneGpsDefaultPort;
  }

  Preferences phone;
  if (!phone.begin(kPhonePrefsNamespace, false)) {
    return;
  }
  if (ssid.length() == 0) {
    phone.clear();  // blank name = no second network
    s_phone_ssid[0] = '\0';
  } else {
    const String old_ssid = phone.getString(kPhoneKeySsid, "");
    phone.putString(kPhoneKeySsid, ssid);
    if (pass.length() > 0 || old_ssid != ssid) {
      phone.putString(kPhoneKeyPass, pass);
    }
    phone.putUShort(kPhoneKeyPort, static_cast<uint16_t>(port));
    strncpy(s_phone_ssid, ssid.c_str(), sizeof(s_phone_ssid) - 1);
    s_phone_ssid[sizeof(s_phone_ssid) - 1] = '\0';
  }
  phone.end();
  Serial.printf("Phone hotspot: \"%s\", GPS port %ld\n", s_phone_ssid, port);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  savePhoneNetworkFromPortal();

  refreshPortalParamValues();
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamValues();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_phone_ssid);
  wm.addParameter(&s_param_phone_pass);
  wm.addParameter(&s_param_gps_port);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  // Was persistent(false): after a wipe/forced portal, the credentials entered
  // in that same boot were then never written to flash, so the NEXT boot asked
  // again. Arduino's default is true; put it back.
  WiFi.persistent(true);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  {
    Preferences phone;
    if (phone.begin(kPhonePrefsNamespace, false)) {
      phone.clear();
      phone.end();
    }
    s_phone_ssid[0] = '\0';
  }
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamValues();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

/**
 * `persist` false (the phone hotspot) keeps these credentials in RAM only, so
 * the flash still holds the HOME network after a reboot or a trip.
 */
void startStaConnect(const String& ssid, const String& pass, bool persist) {
  prepareSta();
  if (!persist) {
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
  }
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
  if (!persist) {
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui,
                      bool persist = true,
                      uint8_t attempts = config::kWifiConnectAttempts) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt, attempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass, persist);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

/** Reads the SSID/password the Wi-Fi stack currently holds (the saved home network). */
bool readStackNetwork(String* ssid, String* pass) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  *ssid = String(ssid_buf);
  *pass = String(pass_buf);
  return true;
}

// The home network as saved by the setup portal. Captured once per boot (and
// after the portal) BEFORE any phone-hotspot connect: that connect keeps its
// credentials in RAM, but esp_wifi_get_config() would then report them.
String s_home_ssid;
String s_home_pass;
bool s_home_captured = false;

void captureHomeNetwork() {
  s_home_ssid = "";
  s_home_pass = "";
  readStackNetwork(&s_home_ssid, &s_home_pass);
  s_home_captured = true;
}

bool loadPhoneNetwork(String* ssid, String* pass) {
  Preferences phone;
  if (!phone.begin(kPhonePrefsNamespace, false)) {
    return false;
  }
  *ssid = phone.getString(kPhoneKeySsid, "");
  *pass = phone.getString(kPhoneKeyPass, "");
  phone.end();
  return ssid->length() > 0;
}

/** Which of the two saved networks are on the air? (Both true if the scan fails.) */
void scanForNetworks(const String& a, const String& b, bool* saw_a, bool* saw_b) {
  *saw_a = true;
  *saw_b = true;
  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }
  *saw_a = false;
  *saw_b = false;
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid == a) *saw_a = true;
    if (ssid == b) *saw_b = true;
  }
  WiFi.scanDelete();
}

bool connectSavedNetwork(bool show_ui) {
  if (!s_home_captured) {
    captureHomeNetwork();
  }
  String phone_ssid;
  String phone_pass;
  const bool have_phone = loadPhoneNetwork(&phone_ssid, &phone_pass) &&
                          phone_ssid != s_home_ssid;
  const bool have_home = s_home_ssid.length() > 0;
  if (!have_home && !have_phone) {
    return false;
  }

  bool home_first = true;
  bool saw_home = true;
  bool saw_phone = true;
  if (have_home && have_phone) {
    // Go by what is on the air, so a trip does not first wait out three failed
    // attempts at the home network (and the reverse).
    scanForNetworks(s_home_ssid, phone_ssid, &saw_home, &saw_phone);
    home_first = !(saw_phone && !saw_home);
  } else if (!have_home) {
    home_first = false;
  }

  // A network that was not seen gets one try instead of three.
  const uint8_t home_tries = saw_home ? config::kWifiConnectAttempts : 1;
  const uint8_t phone_tries = saw_phone ? config::kWifiConnectAttempts : 1;

  for (int round = 0; round < 2; ++round) {
    const bool try_home = (round == 0) == home_first;
    if (try_home) {
      if (have_home &&
          tryConnectWithUi(s_home_ssid, s_home_pass, show_ui, true, home_tries)) {
        return true;
      }
    } else if (have_phone &&
               tryConnectWithUi(phone_ssid, phone_pass, show_ui, false, phone_tries)) {
      return true;
    }
  }
  return false;
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

#if PLANE_RADAR_TOUCH_UI
/** Full-screen "Reset Wi-Fi?" prompt; true if the screen is tapped within 5 s.
 *  On cancel the screen is cleared; the radar repaints itself within 250 ms. */
bool confirmWifiResetByTouch() {
  constexpr unsigned long kConfirmMs = 5000;
  tft.fillScreen(config::kColorYellow);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(config::kTextOnYellow, config::kColorYellow);
  tft.setFont(&fonts::Font4);
  const int cx = tft.width() / 2;
  const int cy = tft.height() / 2;
  tft.drawString("Reset Wi-Fi?", cx, cy - 40);
  tft.setFont(&fonts::Font2);
  tft.drawString("Tap the screen to confirm", cx, cy);
  tft.drawString("Cancels in 5 s", cx, cy + 24);
  while (touchDown()) {
    delay(20);
  }
  bool confirmed = false;
  const unsigned long until = millis() + kConfirmMs;
  while (static_cast<long>(until - millis()) > 0) {
    if (touchDown()) {
      confirmed = true;
      break;
    }
    delay(20);
  }
  tft.fillScreen(static_cast<uint16_t>(0x0000));
  return confirmed;
}
#endif

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
#if PLANE_RADAR_TOUCH_UI
      // Touch boards: on the CYD BOOT is held through every upload (auto-reset doesn't work), so a
      // bare long-press would wipe WiFi + location on each flash. Only act
      // while the radar is up, and only after a tap on the screen confirms.
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("BOOT held — ignored (not connected; portal opens by itself)");
      } else if (confirmWifiResetByTouch()) {
        Serial.println("BOOT held + tap — resetting WiFi");
        wifiResetCredentialsAndReboot();
      } else {
        Serial.println("BOOT held — WiFi reset cancelled (no tap)");
      }
#else
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
#endif
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();
  captureHomeNetwork();  // flash still holds the home network: nothing has connected yet
  {
    String p_ssid, p_pass;
    if (loadPhoneNetwork(&p_ssid, &p_pass)) {
      strncpy(s_phone_ssid, p_ssid.c_str(), sizeof(s_phone_ssid) - 1);
      s_phone_ssid[sizeof(s_phone_ssid) - 1] = '\0';
    }
  }

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      captureHomeNetwork();
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if ((storedWifiCredentials() || s_phone_ssid[0] != '\0') && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    captureHomeNetwork();
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}

bool wifiOnPhoneNetwork() {
  if (s_phone_ssid[0] == '\0' || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  const String ssid = WiFi.SSID();
  return ssid == s_phone_ssid && ssid != s_home_ssid;
}

uint16_t phoneGpsPort() {
  Preferences phone;
  if (!phone.begin(kPhonePrefsNamespace, false)) {
    return config::kPhoneGpsDefaultPort;
  }
  const uint16_t port = phone.getUShort(kPhoneKeyPort, config::kPhoneGpsDefaultPort);
  phone.end();
  return port == 0 ? config::kPhoneGpsDefaultPort : port;
}
