#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPping.h>        // REQUIRED: we will ping all 254 hosts (best-effort) for maximum robustness
#include <Preferences.h>
#include <WiFiUdp.h>
#include <lwip/tcpip.h>
#include <esp_task_wdt.h>

extern "C" {
  #include "esp_netif.h"
  #include "lwip/etharp.h"
  #include "lwip/netif.h"
}

#include <HTTPClient.h>
#include <ArduinoJson.h>

// =======================
// === HARDWARE CONFIG ===
// =======================
#define WDT_TIMEOUT 60
const int RELAY_PIN      = 13;
const int PC_LED_PIN     = 14;
const int STATUS_LED_PIN = 2;   // Built-in LED on many ESP32 boards (often GPIO2)

// =======================
// === DEFAULT SETTINGS ===
// =======================
String httpUser = "changemeuser";
String httpPass = "changemepass";
String workerId = ""; // If empty, we use WiFi hostname (e.g., "esp32-XXXXXX")

// =======================
// === APP CONFIG      ===
// =======================
const size_t MAX_DEVICES          = 32;
const unsigned long SCAN_INTERVAL = 300000UL; // 5 minutes

// =======================
// === GLOBALS         ===
// =======================
WebServer     server(80);
Preferences   prefs;
WiFiUDP       udp;
struct netif* lwip_netif = nullptr;

String deviceList[MAX_DEVICES];
size_t deviceCount = 0;

// Connection state
volatile uint32_t wifiOk = 0;
unsigned long lastStateChange = 0;
const unsigned long MAX_DISCONNECTED_TIME = 900000; // 15 minutes

// LED status control
volatile int blinkInterval = 100; // Fast blink while connecting / AP mode

// Heartbeat
unsigned long lastHeartbeat = 0;
unsigned long heartbeatInterval = 300000; // 5 minutes

// Scan control
volatile uint32_t scanRequested = 0;
TaskHandle_t scanTaskHandle = nullptr;
unsigned long lastScanMillis = 0;

// =======================
// === SIMPLE RATE LIMIT ===
// =======================
// - Applies to ALL endpoints via checkAuth()
// - No delay() used (never blocks loop). We reject until nextOk.
// - Backoff grows by 10x each failed attempt: 5s, 50s 500s
// - Capped to a large max to avoid uint32 overflow / absurd waits.
static const uint8_t  RL_SLOTS  = 32;
static const uint32_t RL_TTL_MS = 49UL * 60UL * 60UL * 1000UL; // 49 hours

static const uint32_t RL_BASE_MS = 5000UL;                // 5 seconds
static const uint32_t RL_MAX_MS  = 48UL * 60UL * 60UL * 1000UL; // 48 hours

struct RateLimitEntry {
  IPAddress ip;
  uint8_t   fails    = 0;
  uint32_t  nextOk   = 0;   // millis() when auth attempts are allowed again
  uint32_t  lastSeen = 0;   // millis() for TTL/LRU
  bool      used     = false;
};

RateLimitEntry rl[RL_SLOTS];

static inline uint32_t rlPenaltyMs(uint8_t fails) {
  // x10 backoff:
  // 1 -> 5s
  // 2 -> 50s
  // 3 -> 500s
  // 4 -> 5000s
  // 5 -> 50000s
  // ... cap at RL_MAX_MS (48h)
  if (fails <= 1) return RL_BASE_MS;

  uint64_t p = RL_BASE_MS;
  for (uint8_t i = 1; i < fails; i++) {
    if (p >= RL_MAX_MS) return RL_MAX_MS;
    p *= 10ULL;
    if (p > RL_MAX_MS) return RL_MAX_MS;
  }
  return (uint32_t)p;
}


static int rlFind(IPAddress ip) {
  for (uint8_t i = 0; i < RL_SLOTS; i++) {
    if (rl[i].used && rl[i].ip == ip) return (int)i;
  }
  return -1;
}

static int rlAlloc(IPAddress ip, uint32_t now) {
  // Free slot
  for (uint8_t i = 0; i < RL_SLOTS; i++) {
    if (!rl[i].used) {
      rl[i].used = true;
      rl[i].ip = ip;
      rl[i].fails = 0;
      rl[i].nextOk = now;
      rl[i].lastSeen = now;
      return (int)i;
    }
  }

  // Recycle expired (TTL)
  for (uint8_t i = 0; i < RL_SLOTS; i++) {
    if (rl[i].used && (uint32_t)(now - rl[i].lastSeen) > RL_TTL_MS) {
      rl[i].used = true;
      rl[i].ip = ip;
      rl[i].fails = 0;
      rl[i].nextOk = now;
      rl[i].lastSeen = now;
      return (int)i;
    }
  }

  // LRU replace
  uint8_t oldest = 0;
  uint32_t oldestSeen = rl[0].lastSeen;
  for (uint8_t i = 1; i < RL_SLOTS; i++) {
    if (rl[i].lastSeen < oldestSeen) {
      oldestSeen = rl[i].lastSeen;
      oldest = i;
    }
  }
  rl[oldest].used = true;
  rl[oldest].ip = ip;
  rl[oldest].fails = 0;
  rl[oldest].nextOk = now;
  rl[oldest].lastSeen = now;
  return (int)oldest;
}

static int rlFindOrCreate(IPAddress ip, uint32_t now) {
  int idx = rlFind(ip);
  if (idx >= 0) return idx;
  return rlAlloc(ip, now);
}

// =======================
// === SETTINGS (NVS)   ===
// =======================
void loadSettings() {
  prefs.begin("cfg", true);
  httpUser = prefs.getString("http_user", "changemeuser");
  httpPass = prefs.getString("http_pass", "changemepass");
  workerId = prefs.getString("worker_id", "");
  prefs.end();

  httpUser.trim();
  httpPass.trim();
  workerId.trim();

  if (httpUser.length() == 0) httpUser = "changemeuser";
  if (httpPass.length() < 4)  httpPass = "changemepass"; // minimal safety net
}

void saveSettings() {
  prefs.begin("cfg", false);
  prefs.putString("http_user", httpUser);
  prefs.putString("http_pass", httpPass);
  prefs.putString("worker_id", workerId);
  prefs.end();
}

// =======================
// === TASK: STATUS LED ===
// =======================
void ledTask(void* pvParameters) {
  pinMode(STATUS_LED_PIN, OUTPUT);
  for (;;) {
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN)); // toggle
    vTaskDelay(pdMS_TO_TICKS(blinkInterval));
  }
}

// =======================
// === AUTH + NETWORK   ===
// =======================
bool checkAuth() {
  IPAddress rip = server.client().remoteIP();
  uint32_t now = millis();

  int idx = rlFindOrCreate(rip, now);
  if (idx >= 0) {
    rl[idx].lastSeen = now;

    // If still penalized, reject quickly (no delay)
    if ((int32_t)(now - rl[idx].nextOk) < 0) {
      server.send(429, "text/plain", "Too many attempts. Try again later.");
      return false;
    }
  }

  if (!server.authenticate(httpUser.c_str(), httpPass.c_str())) {
    if (idx >= 0) {
      if (rl[idx].fails < 255) rl[idx].fails++;
      uint32_t pen = rlPenaltyMs(rl[idx].fails);

      // Compute nextOk safely with uint32 wrap in mind
      rl[idx].nextOk = now + pen;
      rl[idx].lastSeen = now;
    }

    server.requestAuthentication();
    return false;
  }

  // Success => reset counters for this IP
  if (idx >= 0) {
    rl[idx].fails = 0;
    rl[idx].nextOk = now;
    rl[idx].lastSeen = now;
  }
  return true;
}

bool ensureNetif() {
  if (lwip_netif) return true;
  if (wifiOk == 0) return false;

  // Prefer: station netif "st"
  for (struct netif* nif = netif_list; nif; nif = nif->next) {
    if (nif->name[0] == 's' && nif->name[1] == 't') {
      lwip_netif = nif;
      return true;
    }
  }

  // Fallback: match by local IP
  IPAddress lip = WiFi.localIP();
  if (lip == IPAddress(0,0,0,0)) return false;

  ip4_addr_t ipad;
  ipad.addr = htonl(((uint32_t)lip[0] << 24) |
                    ((uint32_t)lip[1] << 16) |
                    ((uint32_t)lip[2] << 8)  |
                    (uint32_t)lip[3]);

  for (struct netif* nif = netif_list; nif; nif = nif->next) {
    if (nif->ip_addr.u_addr.ip4.addr == ipad.addr) {
      lwip_netif = nif;
      return true;
    }
  }

  return false;
}

// =======================
// === WEB PAGE         ===
// =======================
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>PC Control</title>
<style>body{font-family:sans-serif;text-align:center;padding:50px}
button{width:160px;height:42px;font-size:1em;margin:10px}
</style></head><body>
  <h1>PC Power Control</h1>
  <a href="/scan"><button>SCAN NETWORK</button></a>
  <a href="/devices"><button>VIEW DEVICES</button></a>
  <a href="/forget"><button style="background:#555;color:white">FORGET WIFI</button></a>
</body></html>
)rawliteral";

// =======================
// === PERSISTED DEVICES ===
// =======================
void loadDevices() {
  prefs.begin("wol", false);
  deviceCount = prefs.getUInt("count", 0);
  if (deviceCount > MAX_DEVICES) deviceCount = 0;

  for (size_t i = 0; i < deviceCount; ++i) {
    String key = "mac" + String(i);
    deviceList[i] = prefs.getString(key.c_str(), String());
  }
  prefs.end();
}

void saveDevices() {
  prefs.begin("wol", false);
  prefs.putUInt("count", deviceCount);
  for (size_t i = 0; i < deviceCount; ++i) {
    String key = "mac" + String(i);
    prefs.putString(key.c_str(), deviceList[i]);
  }
  prefs.end();
}

bool addDevice(const String& mac) {
  for (size_t i = 0; i < deviceCount; ++i) {
    if (deviceList[i] == mac) return false;
  }
  if (deviceCount < MAX_DEVICES) {
    deviceList[deviceCount++] = mac;
    saveDevices();
    return true;
  }
  return false;
}

// =======================
// === NETWORK SCAN (PING + ARP, ROBUST) ===
// =======================
void doScan() {
  if (!ensureNetif()) {
    Serial.println("❌ Scan skipped: Network interface not ready");
    return;
  }

  IPAddress localIP = WiFi.localIP();
  if (localIP == IPAddress(0,0,0,0)) {
    Serial.println("❌ Scan skipped: No valid IP yet");
    return;
  }

  IPAddress gw = WiFi.gatewayIP();
  String pref = String(localIP[0]) + "." + localIP[1] + "." + localIP[2] + ".";

  Serial.println("🔎 Starting network scan (PING + ARP, robust)...");
  size_t before = deviceCount;

  for (int i = 1; i <= 254; ++i) {
    esp_task_wdt_reset();
    vTaskDelay(1);

    if (wifiOk == 0) break;

    IPAddress tgt;
    tgt.fromString(pref + String(i));

    if (tgt == localIP) continue;
    if (tgt == gw) continue;

    (void)Ping.ping(tgt, 1);

    ip4_addr_t ipa;
    ipa.addr = lwip_htonl(
      ((uint32_t)tgt[0] << 24) |
      ((uint32_t)tgt[1] << 16) |
      ((uint32_t)tgt[2] << 8)  |
       (uint32_t)tgt[3]
    );

    struct eth_addr *mac_ptr = nullptr;
    const ip4_addr_t *ip_ret = nullptr;

    for (int attempt = 0; attempt < 2; ++attempt) {
      LOCK_TCPIP_CORE();
        etharp_request(lwip_netif, &ipa);
        etharp_find_addr(lwip_netif, &ipa, &mac_ptr, &ip_ret);
      UNLOCK_TCPIP_CORE();

      if (mac_ptr && ip_ret) break;
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (mac_ptr && ip_ret) {
      char macBuf[18];
      sprintf(macBuf, "%02X:%02X:%02X:%02X:%02X:%02X",
              mac_ptr->addr[0], mac_ptr->addr[1], mac_ptr->addr[2],
              mac_ptr->addr[3], mac_ptr->addr[4], mac_ptr->addr[5]);

      if (strcmp(macBuf, "00:00:00:00:00:00") != 0 &&
          strcmp(macBuf, "FF:FF:FF:FF:FF:FF") != 0) {
        addDevice(String(macBuf));
      }
    }
  }

  Serial.printf("✅ Scan finished. Added: %u | Total stored: %u\n",
                (unsigned)(deviceCount - before),
                (unsigned)deviceCount);
}

// =======================
// === SCAN TASK        ===
// =======================
void scanTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  vTaskDelay(pdMS_TO_TICKS(30000));
  lastScanMillis = millis();

  for (;;) {
    esp_task_wdt_reset();

    unsigned long now = millis();
    bool stableNetwork = (wifiOk == 1) && (millis() - lastStateChange > 60000);

    if (now - lastScanMillis >= SCAN_INTERVAL) {
      if (stableNetwork) {
        doScan();
        Serial.println("🔄 Automatic scan complete.");
      }
      lastScanMillis = now;
    }

    if (scanRequested == 1) {
      doScan();
      scanRequested = 0;
      lastScanMillis = millis();
      Serial.println("🔄 Manual scan complete.");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =======================
// === WEB HANDLERS     ===
// =======================
void handleRoot() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", PAGE);
}

void handleScan() {
  if (!checkAuth()) return;
  if (scanRequested == 0) scanRequested = 1;
  server.send(200, "text/html",
              "<html><body><h1>Scan requested...</h1><p><a href='/devices'>View list in a few moments</a></p></body></html>");
}

void handleDevices() {
  if (!checkAuth()) return;

  // evitar fragmentación por concatenación
  String html;
  html.reserve(2048);
  html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>WOL list</title></head><body>";
  html += "<h1>WOL Devices</h1><ul>";
  for (size_t i = 0; i < deviceCount; ++i) {
    html += "<li>" + deviceList[i] + " <a href=\"/wake?mac=" + deviceList[i] + "\"><button>WOL</button></a></li>";
  }
  html += "</ul><p><a href=\"/\">Back</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleWake() {
  if (!checkAuth()) return;

  String mac = server.arg("mac");
  uint8_t m[6];
  sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);

  uint8_t pkt[102];
  memset(pkt, 0xFF, 6);
  for (int i = 1; i <= 16; ++i) memcpy(pkt + i * 6, m, 6);

  udp.beginPacket(IPAddress(255, 255, 255, 255), 9);
  udp.write(pkt, sizeof(pkt));
  udp.endPacket();

  server.sendHeader("Location", "/devices");
  server.send(303);
}

void handleClear() {
  if (!checkAuth()) return;

  prefs.begin("wol", false);
  prefs.clear();
  prefs.end();

  deviceCount = 0;
  server.sendHeader("Location", "/devices");
  server.send(303);
}

void handleForget() {
  if (!checkAuth()) return;

  WiFiManager wm;
  wm.resetSettings(); // clears WiFi credentials in flash

  prefs.begin("cfg", false);
  prefs.clear();
  prefs.end();

  server.send(200, "text/html", "<html><body><h1>WiFi forgotten. Restarting...</h1></body></html>");
  delay(1000);
  ESP.restart();
}

// =======================
// === HEARTBEAT        ===
// =======================
bool sendHeartbeat() {
  if (wifiOk == 0) return false;
  if (WiFi.gatewayIP() == IPAddress(0,0,0,0)) return false;

  String url = "https://yourdomain.com/register_worker";
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);

  DynamicJsonDocument doc(512);
  doc["hostname"] = WiFi.getHostname();
  doc["ip"] = WiFi.localIP().toString();

  String wid = workerId.length() ? workerId : String(WiFi.getHostname());
  doc["worker_id"] = wid;

  String payload;
  serializeJson(doc, payload);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);
  bool ok = (httpCode == 200);

  if (ok) Serial.println("✅ Heartbeat OK");
  else    Serial.printf("❌ Heartbeat failed: %d\n", httpCode);

  http.end();
  return ok;
}

// =======================
// === SETUP            ===
// =======================
void setup() {
  Serial.begin(115200);
  delay(500);

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(PC_LED_PIN, INPUT_PULLUP);

  loadSettings();
  loadDevices();

  blinkInterval = 100;
  xTaskCreate(ledTask, "LedTask", 2048, nullptr, 1, nullptr);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.println("✅ GOT_IP: Connected.");
      wifiOk = 1;
      lastStateChange = millis();
      lwip_netif = nullptr;
      blinkInterval = 1000;
    }
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.println("⚠️ DISCONNECTED: WiFi lost.");
      wifiOk = 0;
      lastStateChange = millis();
      lwip_netif = nullptr;
      blinkInterval = 100;
    }
  });

  WiFiManager wm;

  WiFiManagerParameter p_info("<p><b>App settings</b></p>");
  WiFiManagerParameter p_user("http_user", "Web User", httpUser.c_str(), 24);
  WiFiManagerParameter p_pass("http_pass", "Web Password", httpPass.c_str(), 32);
  WiFiManagerParameter p_wid ("worker_id", "Worker ID (heartbeat)", workerId.c_str(), 32);

  wm.addParameter(&p_info);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_wid);

  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);

  wm.setSaveConfigCallback([&]() {
    httpUser = String(p_user.getValue()); httpUser.trim();
    httpPass = String(p_pass.getValue()); httpPass.trim();
    workerId = String(p_wid.getValue());  workerId.trim();

    if (httpUser.length() == 0) httpUser = "changemeuser";
    if (httpPass.length() < 4)  httpPass = "changemepass";

    saveSettings();
    Serial.println("✅ App settings saved from portal");
  });

  Serial.println("🔄 Starting WiFiManager...");

  esp_task_wdt_delete(NULL);
  bool wifiConnected = wm.autoConnect("ControlPC_AP", "changeme1234");
  esp_task_wdt_add(NULL);

  if (!wifiConnected) {
    Serial.println("❌ WiFiManager failed (timeout). Restarting...");
    delay(1000);
    ESP.restart();
  }

  wifiOk = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  blinkInterval = wifiOk ? 1000 : 100;
  lastStateChange = millis();
  lwip_netif = nullptr;

  Serial.print("🚀 Connected. IP: ");
  Serial.println(WiFi.localIP());

  udp.begin(9);

  server.on("/",        handleRoot);
  server.on("/scan",    handleScan);
  server.on("/devices", handleDevices);
  server.on("/wake",    handleWake);
  server.on("/clear",   handleClear);
  server.on("/forget",  handleForget);
  server.begin();

  xTaskCreatePinnedToCore(scanTask, "ScanTask", 8192, nullptr, 1, &scanTaskHandle, 1);
}

// =======================
// === LOOP            ===
// =======================
void loop() {
  esp_task_wdt_reset();

  if (wifiOk == 1) {
    server.handleClient();
  } else {
    if (millis() - lastStateChange > MAX_DISCONNECTED_TIME) {
      Serial.println("❌ Disconnected timeout exceeded. Restarting...");
      delay(100);
      ESP.restart();
    }
  }

  if (wifiOk == 1 && millis() - lastHeartbeat >= heartbeatInterval) {
    lastHeartbeat = millis();

    bool ok = sendHeartbeat();
    if (ok) {
      heartbeatInterval = 300000;
      Serial.println("💤 Next heartbeat in 5 min.");
    } else {
      heartbeatInterval = 30000;
      Serial.println("⚠️ Heartbeat failed. Retrying in 30 sec.");
    }
  }
}
