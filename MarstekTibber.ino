// -----------------------------------------------------------------------------
// MarstekTibber_260614
// -----------------------------------------------------------------------------
// ESP32 implementation for a Shelly Pro 3EM-style emulator on the
// Lilygo T-Display. The goal is to provide a stable UDP surface that a
// Marstek battery can discover and poll actual consumption data from the 
// energy meter.
//
// Notes:
// - The sketch intentionally kept comparable to the AstraMeter by Tomquist.
// - It is intended for users, who do not want to run HomeAssistant on a more
//   power consuming hardware.
// - The power consumption value is stored in a global variable and is
//   updated by a function that reads Tibber Pulse.
// - "decayPowBetweenSamples" got introduced to avoid oscillation. The 
//   Marstek battery is polling each second, while TibberPulse deliveres samples 
//   only after 3+ seconds. Without the decay the battery will further steer its 
//   output over/under the required power. Aim is, to tell the battery some
//   "successful" reaction in advance of the next real measurement. But if the 
//   value is too small (fast decay) the battery sometimes does not output power
//   at all.
// - ShellyProEM3 data im Marstek B2500-D v116.6 has a dead time interval of 
//   15 to 25 seconds. On ShellyPro EM-50 data the Marstek firmware reacts 
//   much faster.
// -----------------------------------------------------------------------------
// MarstekTibber_260910
// -----------------------------------------------------------------------------
// Support to enable battery only at price peaks. 
// - number of most expensive intervals can be set in prices.h
// - the window for peak detection will be set from 2 p.m to 2 p.m. of the 
//   next day
// Support for multiple batteries included.
// -----------------------------------------------------------------------------
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <base64.h>

#include "configuration.h"
#include "display.h"
#include "prices.h"
#include "index_html.h"


// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
WebServer              server(80);
WiFiUDP                udpOld;
WiFiUDP                udpNew;
WiFiUDP                udpEm50;

volatile int           g_currentPowerWatts     = 0;
volatile unsigned long g_lastPowerUpdateMs     = 0;
volatile unsigned long g_priceScreenTriggerMs  = 1; 

// 15-min consumption average  int32_t avgPowerWatts;
int32_t                g_pulsePowerSumInInterval = 0;
uint32_t               g_pulseSamplesCount       = 0;

TaskHandle_t           tibberTaskHandle        = NULL;
TaskHandle_t           displayTaskHandle       = NULL;

volatile bool          triggerScreenRefresh    = false;
volatile bool          tibberConnected         = false;
volatile unsigned long pulseOffAtMs            = 0;

// -----------------------------------------------------------------------------
// Dynamische Batterie-Registrierung
// -----------------------------------------------------------------------------
volatile int  f_currentPowerWatts[kMaxBatteries] = {0, 0, 0, 0}; 
IPAddress     g_batteryIPs[kMaxBatteries];          // Speichert die IPs der erkannten Batterien
volatile unsigned long 
    g_batteryTimers[kMaxBatteries] = {0, 0, 0, 0};  // Flash-Timer pro Batterie
volatile int g_registeredBatteriesCount = 0;        // Aktuelle Anzahl gefundener Batterien

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
String jsonStatus() {
  char buffer[384];
  snprintf(buffer, sizeof(buffer),
    "{\n"
    "  \"wifi_sta\": {\n"
    "    \"connected\": true,\n"
    "    \"ssid\": \"%s\",\n"
    "    \"ip\": \"%s\"\n"
    "  },\n"
    "  \"meters\": [\n"
    "    {\n"
    "      \"power\": %d\n"
    "    }\n"
    "  ]\n"
    "}\n",
    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), g_currentPowerWatts
  );
  return String(buffer);
}

String jsonEmStatus() {
  char buffer[256];
  snprintf(buffer, sizeof(buffer),
    "{\n"
    "  \"id\": 0,\n"
    "  \"total_act_power\": %d,\n"
    "  \"total_act_energy\": 0,\n"
    "  \"total_act_ret_power\": 0,\n"
    "  \"total_act_ret_energy\": 0,\n"
    "  \"voltage\": 230.0,\n"
    "  \"current\": 0.0,\n"
    "  \"freq\": 50.0\n"
    "}\n",
    g_currentPowerWatts
  );
  return String(buffer);
}

String jsonEm1Status() {
  const float total         = static_cast<float>(g_currentPowerWatts);
  const float voltage       = 230.0f;
  const float current       = fabsf(total) / voltage;
  const float pf            = (total == 0.0f) ? 1.0f : 0.95f;
  const float apparentPower = fabsf(total) / pf;
  char buffer[256];
  snprintf(buffer, sizeof(buffer),
    "{\"id\":0,\"src\":\"%s\",\"result\":{"
    "\"id\":0,"
    "\"current\":%.3f,"
    "\"voltage\":%.1f,"
    "\"act_power\":%.3f,"
    "\"aprt_power\":%.1f,"
    "\"pf\":%.2f,"
    "\"freq\":50.0"
    "}}",
    kDeviceId, current, voltage, total, apparentPower, pf
  );
  return String(buffer);
}

// Extrahiert den Wert eines Schlüssels direkt aus dem rohen Request-String (fragmentsicher)
String extractJsonString(const String& json, const char* key) {
  const char* raw = json.c_str();
  
  // Suchen nach dem Key im Format: "key"
  char searchKey[32];
  snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);
  
  const char* match = strstr(raw, searchKey);
  if (!match) return String();

  // Zum Doppelpunkt nach dem Key springen
  match = strstr(match, ":");
  if (!match) return String();
  match++; // Hinter den Doppelpunkt springen

  // Whitespaces überspringen
  while (*match == ' ' || *match == '\t' || *match == '\n' || *match == '\r') {
    match++;
  }

  // Wenn es ein String-Wert ist (beginnt mit Anführungszeichen)
  if (*match == '"') {
    match++; // Start hinter dem ersten "
    const char* end = strstr(match, "\"");
    if (!end) return String();
    
    // Kopiert nur den exakten Ausschnitt temporär in ein String-Objekt
    char valBuffer[64] = {0};
    size_t len = end - match;
    if (len >= sizeof(valBuffer)) len = sizeof(valBuffer) - 1;
    strncpy(valBuffer, match, len);
    return String(valBuffer);
  }

  // Wenn es ein numerischer Wert oder Boolean ist
  const char* end = match;
  while (*end != '\0' && *end != ',' && *end != '}' && *end != ']') {
    end++;
  }
  
  char valBuffer[64] = {0};
  size_t len = end - match;
  if (len >= sizeof(valBuffer)) len = sizeof(valBuffer) - 1;
  strncpy(valBuffer, match, len);
  return String(valBuffer);
}

// Direkte, hochperformante Extraktion für Integer (wie die Request-ID)
int extractJsonInt(const String& json, const char* key) {
  const char* raw = json.c_str();
  char searchKey[32];
  snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);
  
  const char* match = strstr(raw, searchKey);
  if (!match) return 0;
  
  match = strstr(match, ":");
  if (!match) return 0;
  match++;
  
  // atoi parst die Zahl direkt aus dem Zeiger heraus, ignoriert Leerzeichen automatisch!
  return atoi(match); 
}


// -----------------------------------------------------------------------------
// UDP response formatting for Marstek B2500-D
// -----------------------------------------------------------------------------
String buildUdpResponse(const String& request, int batteryIndex) {
  const int requestId = extractJsonInt(request, "id");
  const String method = extractJsonString(request, "method");

  // invalid index
  if (batteryIndex < 0 || batteryIndex >= kMaxBatteries) {
    return String();
  }
  
  // The indexed battery only needs the JSON response and the requested power values.
  float total = 0.0f;
  if (isCurrentIntervalExpensive()) {          // check Tibber prices
    // Tell the battery the truth (Nulleinspeisung)
    total = static_cast<float>(f_currentPowerWatts[batteryIndex]);

    // Next sample for UDP in ~1 second: let fake samples decline with 
    // alpha 0.7 ... 0.9 until we get fresh value from tibber pulse
    f_currentPowerWatts[batteryIndex] = static_cast<int>(static_cast<float>(
				    f_currentPowerWatts[batteryIndex]) * decayPowBetweenSamples);
  } else {
    // Prices are moderate or cheap: mimic export situation
    total = -10.0f;
  }


  // Reserve buffer at stack (no heap)
  char jsonBuffer[384];

  // --- Method 1: Protocol for Shelly Pro 3EM ---
  if (method == "EM.GetStatus") {
      // Phase A used only, Phases B+C are dummy
      const float a  = total;
      const float b  = 0.0f;
      const float c  = 0.0f;
      
      // How many decimals (3 for small values close to 0, otherwise 1)
      const int aDec = fabsf(a) < 0.1f ? 3 : 1;
      const int bDec = fabsf(b) < 0.1f ? 3 : 1;
      const int cDec = fabsf(c) < 0.1f ? 3 : 1;

      // "%.*f" allows dynamic steering of decimals by variable
      snprintf(jsonBuffer, sizeof(jsonBuffer),
        "{\"id\":%d,\"src\":\"%s\",\"dst\":\"unknown\",\"result\":{"
        "\"a_act_power\":%.*f,"
        "\"b_act_power\":%.*f,"
        "\"c_act_power\":%.*f,"
        "\"total_act_power\":%.3f"
        "}}",
        requestId, kDeviceId, aDec, a, bDec, b, cDec, c, total
      );
      
      return String(jsonBuffer);
  }

  // --- Method 2: Protocol for Shelly Pro EM-50 (faster with B2500-D v116.6) ---
  if (method == "EM1.GetStatus") {
      const float voltage       = 230.0f;
      const float current       = fabsf(total) / voltage;
      const float pf            = (total == 0.0f) ? 1.0f : 0.95f;
      const float apparentPower = fabsf(total) / pf;

      snprintf(jsonBuffer, sizeof(jsonBuffer),
        "{\"id\":%d,\"src\":\"%s\",\"result\":{"
        "\"id\":0,"
        "\"current\":%.3f,"
        "\"voltage\":%.1f,"
        "\"act_power\":%.3f,"
        "\"aprt_power\":%.1f,"
        "\"pf\":%.2f,"
        "\"freq\":50.0"
        "}}",
        requestId, kDeviceId, current, voltage, total, apparentPower, pf
      );

      return String(jsonBuffer);
  }

  return String(); // none of the above methods.
}

void handleUdpPacket(WiFiUDP& udp, const uint16_t port) {
  const int packetLen = udp.parsePacket();
  if (packetLen <= 0) return;

  char packet[256];
  int len = udp.read(packet, sizeof(packet) - 1);
  if (len <= 0) return;
  packet[len] = '\0';

  String request(packet);
  IPAddress remoteIP = udp.remoteIP();

  // ---- Dynamic assignment ----
  int batteryIndex = -1;
  
  // Search, if IP already registered
  for (int i = 0; i < g_registeredBatteriesCount; i++) {
      if (g_batteryIPs[i] == remoteIP) {
          batteryIndex = i;
          break;
      }
  }

  // register, if IP is new and space left
  if (batteryIndex == -1 && g_registeredBatteriesCount < kMaxBatteries) {
      batteryIndex = g_registeredBatteriesCount;
      g_batteryIPs[batteryIndex] = remoteIP;
      g_registeredBatteriesCount++;
      Serial.printf("[MarstekTibber] New Battery %d registered with IP: %s\n", 
                    g_registeredBatteriesCount, remoteIP.toString().c_str());
  }

  // If battery was successfully assigned (now or before)
  if (batteryIndex != -1) {
      g_batteryTimers[batteryIndex] = millis() + 250; // Set flash timer for the specific "M"
  } else {
    return; // array full
  }

  // Build specific response for this battery
  String response = buildUdpResponse(request, batteryIndex);
  if (response.length() == 0) return;

  udp.beginPacket(remoteIP, udp.remotePort());
  udp.printf("%s", response.c_str());
  udp.endPacket();

  triggerScreenRefresh = true;

  //Serial.print("[MarstekTibber] UDP response on port ");
  //Serial.print(port);
  //Serial.print(" for ");
  //Serial.println(request.substring(0, min((int)request.length(), 120)));
}

// -----------------------------------------------------------------------------
// Aggregiert die echten Pulse-Rohwerte für das aktuelle 15-Minuten-Intervall
// -----------------------------------------------------------------------------
// Keep track of the last interval index we processed
int g_lastIntervalIndex = -1;

void aggregate_power(int currentWatts) {
  time_t nowTime = time(nullptr);
  int currentIntervalIndex = -1;

  // 1. Find the current 15-minute interval
  for (int i = 0; i < g_activeIntervalsCount; i++) {
    if (nowTime >= g_priceIntervals[i].startEpoch && nowTime < (g_priceIntervals[i].startEpoch + 900)) {
      currentIntervalIndex = i;
      break;
    }
  }

  // If no matching interval is found, we cannot aggregate safely
  if (currentIntervalIndex == -1) {
    return;
  }

  // 2. Detect an interval switch: Reset counters if we just crossed into a new 15-min window
  if (currentIntervalIndex != g_lastIntervalIndex) {
    g_pulsePowerSumInInterval = 0;
    g_pulseSamplesCount = 0;
    g_lastIntervalIndex = currentIntervalIndex;
  }

  // 3. Accumulate and calculate the true interval average
  g_pulsePowerSumInInterval += currentWatts;
  g_pulseSamplesCount++;

  g_priceIntervals[currentIntervalIndex].avgPowerWatts = g_pulsePowerSumInInterval / g_pulseSamplesCount;
}


// -----------------------------------------------------------------------------
// Reading meter data from Pulse via Tibber Bridge
// -----------------------------------------------------------------------------
void tibber_polling_task(void *parameter) {
  HTTPClient http;
  Serial.println("Starting SML-Parser...");
  
  // Startet mit deiner konfigurierten ID (oder standardmäßig 1)
  int active_node_id = tibber_node_id; 
  if (active_node_id <= 0) active_node_id = 1;

  // Speichert die letzte ID, um die URL fragmentsicher und ohne Heap-Last zu verwalten
  int last_applied_node_id = -1; 
  char sml_url_buffer[128] = {0};
  char nodes_url_buffer[128] = {0};

  // Die Nodes-URL bleibt statisch
  snprintf(nodes_url_buffer, sizeof(nodes_url_buffer), "http://%s/nodes.json", tibber_bridge_ip);

  // Einmaliges, fragmentsicheres Berechnen der Tibber-Verbindungsdaten vor dem Loop-Start
  const String auth_string = "admin:" + String(tibber_bridge_password);
  const String auth_base64 = "Basic " + base64::encode((uint8_t*)auth_string.c_str(), auth_string.length());

  // Schutz vor permanentem Polling der nodes.json bei totalem Verbindungsabbruch
  unsigned long nextAllowedDiscoveryMs = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {

      // URL wird nur bei einer echten ID-Änderung (oder beim Start) fragmentsicher neu gebaut
      if (active_node_id != last_applied_node_id) {
        snprintf(sml_url_buffer, sizeof(sml_url_buffer), "http://%s/node_data.json?node_id=%d", tibber_bridge_ip, active_node_id);
        last_applied_node_id = active_node_id;
      }

      http.begin(String(sml_url_buffer));
      http.addHeader("Authorization", auth_base64);
      http.addHeader("Connection", "close"); // Wichtig: Verbindung sofort schließen

      // Angemessene Timeouts
      http.setConnectTimeout(3000);
      http.setTimeout(3000);

      int httpCode = http.GET();
      if (httpCode == 200) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[1024];
        int           len          = 0;
        unsigned long startRead    = millis();
        unsigned long lastByteTime = millis();
        
        while(http.connected() && (len < sizeof(buffer))) {
          if (stream && stream->available()) {
            buffer[len++] = stream->read();
            lastByteTime  = millis();
          } else { 
            vTaskDelay(pdMS_TO_TICKS(5));
          } 
          if (millis() - lastByteTime > 300) break;
          if (millis() - startRead > 2500) break;
        } 

        if (len > 50) {
          int power_index = -1;
          for (int i = 0; i < len - 20; i++) {
            if (buffer[i+0] == 0x01 && buffer[i+1] == 0x00 && buffer[i+2] == 0x10 &&
                buffer[i+3] == 0x07 && buffer[i+4] == 0x00 && buffer[i+5] == 0xFF) {
              power_index = i;
              break;
            }
          }

          if (power_index != -1 && (power_index + 20) < len) {
            int8_t  scaler     = (int8_t)buffer[power_index + 15];
            float   multiplier = (scaler == -1) ? 0.1 : (scaler == -2) ? 0.01 : 0.001;
            uint8_t valType    = buffer[power_index + 16];
            int     valIdx     = power_index + 17;
            int32_t raw_power  = 0;
            
			if (valType == 0x53) { 
				// Sicheres Verodern von zwei isolierten Bytes als signed 16-Bit
				int16_t intermediate = (int16_t)(((uint16_t)(buffer[valIdx + 0] & 0xFF) << 8) | 
												  (uint16_t)(buffer[valIdx + 1] & 0xFF));
				raw_power = (int32_t)intermediate;
			}
			else if (valType == 0x54) {
				// 24-Bit Wert sauber zusammenbauen (ohne Geisterbits in den oberen Etagen)
				uint32_t val24 = (((uint32_t)(buffer[valIdx + 0] & 0xFF)) << 16) |
								 (((uint32_t)(buffer[valIdx + 1] & 0xFF)) <<  8) |
								  ((uint32_t)(buffer[valIdx + 2] & 0xFF));
								 
				// Korrekte mathematische Sign-Extension für 24-Bit zu signed 32-Bit
				if (val24 & 0x800000) {
					raw_power = (int32_t)(val24 | 0xFF000000);
				} else {
					raw_power = (int32_t)(val24 & 0x00FFFFFF);
				}
			}
			else if (valType == 0x55) {
				// Absolut sauberes Zusammenfügen eines echten 32-Bit Ints
				raw_power = (((int32_t)(buffer[valIdx + 0] & 0xFF)) << 24) |
							(((int32_t)(buffer[valIdx + 1] & 0xFF)) << 16) |
							(((int32_t)(buffer[valIdx + 2] & 0xFF)) <<  8) |
							 ((int32_t)(buffer[valIdx + 3] & 0xFF));
			}

            g_currentPowerWatts   = (int)(static_cast<float>(raw_power) * multiplier);
            for (int i = 0; i < kMaxBatteries; i++) {
              f_currentPowerWatts[i] = g_currentPowerWatts;
            }

            g_lastPowerUpdateMs   = millis();
            
            aggregate_power(g_currentPowerWatts);

            // initial 15 s show price screen
            if (g_showInitialPrices && g_activeIntervalsCount > 0 && millis() > 15000) {
              g_showInitialPrices = false;
            }

            // write to ring buffer
            g_powerHistory[g_historyIndex] = g_currentPowerWatts;
            g_historyIndex = (g_historyIndex + 1) % kGraphMaxSamples;
            if (g_historyCount < kGraphMaxSamples) {
              g_historyCount++;
            }

            Serial.printf("[Tibber] @%lu ms, Power: %d W \n", g_lastPowerUpdateMs, g_currentPowerWatts);

            tibberConnected       = true;
            pulseOffAtMs          = g_lastPowerUpdateMs + 250; 
            triggerScreenRefresh  = true;

            http.end();
            vTaskDelay(pdMS_TO_TICKS(tibberIntervalMs));
          }  else {
            // SML-Parsing fehlgeschlagen trotz HTTP 200
            http.end();
            vTaskDelay(pdMS_TO_TICKS(tibberIntervalMs));
          }
        } else {
          // Zu wenig Daten empfangen
          http.end();
          vTaskDelay(pdMS_TO_TICKS(tibberIntervalMs));
        }
      } else {
        // if bridge denies access, wait longer to recover
        Serial.printf("[Tibber] @%lu ms, Bridge Fehler: %d. Warte auf Freigabe...\n", millis(), httpCode);
        tibberConnected       = false;
        triggerScreenRefresh  = true;
        http.end();

        // Einmalige automatische Recovery-Erkennung im Fehlerfall (Schutz vor permanentem Polling: max. alle 60 Sek)
        if (millis() > nextAllowedDiscoveryMs) {
          HTTPClient discoveryHttp;
          discoveryHttp.begin(String(nodes_url_buffer));
          discoveryHttp.addHeader("Authorization", auth_base64);
          discoveryHttp.addHeader("Connection", "close");
          discoveryHttp.setConnectTimeout(3000);
          discoveryHttp.setTimeout(3000);
          
          int discCode = discoveryHttp.GET();
          if (discCode == 200) {
            String payload = discoveryHttp.getString();
            int parsed_id = extractJsonInt(payload, "node_id");
            if (parsed_id > 0 && parsed_id != active_node_id) {
              active_node_id = parsed_id; // ID wird zur Laufzeit ohne Heap-Fragmentierung angepasst
              Serial.printf("[Tibber] Node-ID shifted dynamically to: %d\n", active_node_id);
            }
          }
          discoveryHttp.end();
          nextAllowedDiscoveryMs = millis() + 60000;
        }

        vTaskDelay(pdMS_TO_TICKS(6000));
      }
	  // guaranteed closure of the socket through whatever path we get here
	  http.end();

      checkAndFetchPrices(); // Is a new price update required?

	  vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      // Keine WLAN-Verbindung
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } 
}


// -----------------------------------------------------------------------------
// HTTP endpoints (to connect other equipment, not used for Marstek B2500)
// -----------------------------------------------------------------------------
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  // Liefert die HTML-Seite mit dem korrekten Content-Type aus
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", jsonStatus());
}

void handleRpcEmGetStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", jsonEmStatus());
}

void handleRpcEm1GetStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", jsonEm1Status());
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}\n");
}

void handleGetPrices() {
    streamPriceIntervalsJson(server);
}

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  setCpuFrequencyMhz(80); 
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  setup_display(tibber_bridge_ip);
  Serial.println("\n[MarstekTibber] Starting Shelly emulation");

  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);

  Serial.print("[MarstekTibber] Connecting to Wi-Fi");
  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 30000) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[MarstekTibber] Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    ip_info_display(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[MarstekTibber] Wi-Fi connection timed out; continuing anyway.");
  }

  initPrices(); 

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/prices",             HTTP_GET, handleGetPrices);
  server.on("/rpc/EM.GetStatus",   HTTP_GET, handleRpcEmGetStatus);
  server.on("/rpc/EM.GetStatus/",  HTTP_GET, handleRpcEmGetStatus);
  server.on("/rpc/EM1.GetStatus",  HTTP_GET, handleRpcEm1GetStatus);
  server.on("/rpc/EM1.GetStatus/", HTTP_GET, handleRpcEm1GetStatus);
  server.onNotFound(handleNotFound);

  Serial.println("[MarstekTibber] starting HTTP server.");
  server.begin();

  Serial.println("[MarstekTibber] starting UDP listeners on ports 1010, 2220 and 2223");
  udpOld.begin(1010);
  udpNew.begin(2220);
  udpEm50.begin(2223);

  Serial.println("[MarstekTibber] Wait before polling Tibber Bridge...");
  delay(5000); 

  // Core 0: RTOS, WiFi stack, Display
  xTaskCreatePinnedToCore(display_management_task, "DisplayTask",   4096, NULL, 1, &displayTaskHandle, 0);
  // Core 1: Arduino-loop, UDP, Tibber
  xTaskCreatePinnedToCore(tibber_polling_task,     "TibberPolling", 8192, NULL, 1, &tibberTaskHandle,  1);
}

void loop() {
  server.handleClient();

  handleUdpPacket(udpOld,  1010);
  handleUdpPacket(udpNew,  2220);
  handleUdpPacket(udpEm50, 2223);

  // Button for price screen
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!g_showInitialPrices && g_priceScreenTriggerMs == 0) {
      g_showInitialPrices    = true;
      triggerScreenRefresh   = true;
      g_priceScreenTriggerMs = millis();
    }
    delay(50); // de-bounce
  }
  
  vTaskDelay(pdMS_TO_TICKS(1));
}

