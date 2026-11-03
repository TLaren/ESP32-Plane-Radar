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

/**
 * WiFiManager keeps its back-button flag and menu state protected. Two portals
 * share one instance:
 *  - the setup network (first run / after a reset): Wi-Fi page with the other
 *    settings on it, as upstream;
 *  - the LAN page while connected: Setup / Info only. The Wi-Fi page is left out
 *    on purpose: saving it re-connects with whatever is in its boxes (a blank
 *    password drops a WPA network) and it is pre-filled with the CURRENT network,
 *    which on a trip is the phone hotspot.
 */
class PortalManager : public WiFiManager {
 public:
  void enableBackButton() { _showBack = true; }
  void useSetupMenu() {
    _paramsInWifi = true;
    setMenu(_menuIdsDefault);
  }
  void useLanMenu() {
    static const char* kMenu[] = {"param", "info", "exit"};
    setMenu(kMenu, 3);  // "param" moves the settings to their own page
  }
};

bool s_force_config_portal = false;
PortalManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void onPortalWifiSaved();
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

// Both networks live in our own NVS namespace (home + the phone's hotspot). The
// ESP-IDF flash config is only used to import the home network once; after that
// it can hold whatever network was joined last without mattering.
constexpr char kPhonePrefsNamespace[] = "wifi2";
constexpr char kPhoneKeySsid[] = "ssid";
constexpr char kPhoneKeyPass[] = "pass";
constexpr char kPhoneKeyPort[] = "port";
constexpr char kHomeKeySsid[] = "home_ssid";
constexpr char kHomeKeyPass[] = "home_pass";

WiFiManagerParameter s_param_travel_head(
    "<br/><hr><b>Phone hotspot for trips (optional)</b><br/>");

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
  wm.addParameter(&s_param_travel_head);
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
  s_wm.setSaveConfigCallback(onPortalWifiSaved);
  s_wm.enableBackButton();
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
  s_wm.useLanMenu();
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

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
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

    startStaConnect(ssid, pass);

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

// Home network: our own copy in NVS. Written when the setup portal joins a
// network (see onPortalWifiSaved), and imported once from the stack's flash
// config the first time this firmware runs.
String s_home_ssid;
String s_home_pass;

void loadHomeNetwork() {
  Preferences prefs;
  if (!prefs.begin(kPhonePrefsNamespace, false)) {
    return;
  }
  s_home_ssid = prefs.getString(kHomeKeySsid, "");
  s_home_pass = prefs.getString(kHomeKeyPass, "");
  prefs.end();
}

void storeHomeNetwork(const String& ssid, const String& pass) {
  Preferences prefs;
  if (prefs.begin(kPhonePrefsNamespace, false)) {
    prefs.putString(kHomeKeySsid, ssid);
    prefs.putString(kHomeKeyPass, pass);
    prefs.end();
  }
  s_home_ssid = ssid;
  s_home_pass = pass;
  Serial.printf("Home Wi-Fi saved: \"%s\"\n", ssid.c_str());
}

/** Remember the network we are connected to as HOME (never the phone hotspot). */
void rememberHomeFromLink() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  const String ssid = WiFi.SSID();
  if (ssid.length() == 0 || ssid == s_phone_ssid) {
    return;
  }
  storeHomeNetwork(ssid, WiFi.psk());
}

void onPortalWifiSaved() { rememberHomeFromLink(); }

void importHomeFromFlashOnce() {
  if (s_home_ssid.length() > 0) {
    return;
  }
  String ssid;
  String pass;
  if (readStackNetwork(&ssid, &pass) && ssid != s_phone_ssid) {
    storeHomeNetwork(ssid, pass);
  }
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

enum class ConnectResult { Connected, NotInRange, Failed };

/** Which saved networks are on the air? Returns false if the scan did not work. */
bool scanForNetworks(const String& home, const String& phone, bool* saw_home,
                     bool* saw_phone) {
  *saw_home = false;
  *saw_phone = false;
  // Stop the stack retrying the old network first: a scan started while it is
  // mid-connect fails, and the old code then fell back to 3 x 15 s of retries.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(150);
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    delay(500);
    n = WiFi.scanNetworks(false, true);
  }
  if (n < 0) {
    WiFi.scanDelete();
    return false;
  }
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (home.length() > 0 && ssid == home) *saw_home = true;
    if (phone.length() > 0 && ssid == phone) *saw_phone = true;
  }
  WiFi.scanDelete();
  return true;
}

/**
 * Join the home network if it is in range, else the phone hotspot. A network
 * that is not on the air is never tried, so out of range of both this returns
 * after one scan (NotInRange) instead of retrying for minutes.
 */
ConnectResult connectSavedNetwork(bool show_ui) {
  String phone_ssid;
  String phone_pass;
  const bool have_phone = loadPhoneNetwork(&phone_ssid, &phone_pass) &&
                          phone_ssid != s_home_ssid;
  const bool have_home = s_home_ssid.length() > 0;
  if (!have_home && !have_phone) {
    return ConnectResult::Failed;
  }

  if (show_ui) {
    statusScreenConnectingBegin("a saved Wi-Fi");
  }
  bool saw_home = have_home;
  bool saw_phone = have_phone;
  if (!scanForNetworks(s_home_ssid, phone_ssid, &saw_home, &saw_phone)) {
    // Scan unusable: assume both are there and let the connect attempts decide.
    saw_home = have_home;
    saw_phone = have_phone;
  }
  Serial.printf("Wi-Fi scan: home \"%s\" %s, phone \"%s\" %s\n",
                s_home_ssid.c_str(), saw_home ? "seen" : "not seen",
                phone_ssid.c_str(), saw_phone ? "seen" : "not seen");

  bool tried = false;
  if (have_home && saw_home) {
    tried = true;
    if (tryConnectWithUi(s_home_ssid, s_home_pass, show_ui, 2)) {
      return ConnectResult::Connected;
    }
  }
  if (have_phone && saw_phone) {
    tried = true;
    if (tryConnectWithUi(phone_ssid, phone_pass, show_ui, 2)) {
      return ConnectResult::Connected;
    }
  }
  return tried ? ConnectResult::Failed : ConnectResult::NotInRange;
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.useSetupMenu();
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
      // bare long-press would wipe WiFi + location on each flash. It only acts
      // after a tap on the screen confirms. That also works while NOT
      // connected, which is the way out when no saved network is in range.
      if (confirmWifiResetByTouch()) {
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
  return connectSavedNetwork(true) == ConnectResult::Connected;
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
  {
    String p_ssid, p_pass;
    if (loadPhoneNetwork(&p_ssid, &p_pass)) {
      strncpy(s_phone_ssid, p_ssid.c_str(), sizeof(s_phone_ssid) - 1);
      s_phone_ssid[sizeof(s_phone_ssid) - 1] = '\0';
    }
  }
  loadHomeNetwork();
  importHomeFromFlashOnce();  // first run of this firmware: keep the saved home Wi-Fi

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
      rememberHomeFromLink();
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

  String probe_ssid;
  String probe_pass;
  const bool have_saved = s_home_ssid.length() > 0 || loadPhoneNetwork(&probe_ssid, &probe_pass);
  if (have_saved) {
    const ConnectResult r = connectSavedNetwork(true);
    if (r == ConnectResult::Connected) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    if (r == ConnectResult::NotInRange) {
      // Normal on a trip (hotspot not on yet, home far away): do not open the
      // setup portal, the main loop keeps looking. BOOT 3 s + tap = way out.
      Serial.println("No saved Wi-Fi in range — will keep looking");
      return false;
    }
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    rememberHomeFromLink();
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
