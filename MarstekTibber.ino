// -----------------------------------------------------------------------------
// MarstekTibber_260614
// -----------------------------------------------------------------------------
// ESP32 implementation for a Shelly Pro 3EM-style emulator on the
// Lilygo T-Display. The goal is to provide a stable UDP surface that a
// Marstek battery can discover and poll actual consumption data from the 
// energy meter.
//
// Notes:
// - The sketch intentionally kept compatible to the AstraMeter by Tomquist.
// - It is intended for users, who do not want to run HomeAssistant at a more
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
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <base64.h>

#include "display.h"

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
// Replace these with your own values before flashing the device.
constexpr char      kWifiSsid[]             = "WiFi_SSID";
constexpr char      kWifiPassword[]         = "WiFi_password";
//constexpr char    kDeviceId[]             = "shellypro3em-ec4609c439c1";
constexpr char      kDeviceId[]             = "shellyproem50-ec4609c439c2";

const char*         tibber_bridge_ip        = "192.168.178.65";
const char*         tibber_bridge_password  = "B2CG-B***"; // code beside the QR
const int           tibber_node_id          = 1;
const unsigned long tibberIntervalMs        = 3000;

// 1. if tibber connection lost decay power slowly over 5 minutes
// 2. in between samples a slight decay might help against nervouse polling within
//    dead time of tibber pulse
const float         decayPowBetweenSamples  = 0.80f; // next sample 80% of actual

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
WebServer              server(80);
WiFiUDP                udpOld;
WiFiUDP                udpNew;
WiFiUDP                udpEm50;

volatile int           g_currentPowerWatts  = 0;
volatile int           f_currentPowerWatts  = 0;
volatile unsigned long g_lastPowerUpdateMs  = 0;

TaskHandle_t           tibberTaskHandle     = NULL;
TaskHandle_t           displayTaskHandle    = NULL;

volatile bool          triggerScreenRefresh = false;
volatile bool          tibberConnected      = false;
volatile unsigned long pulseOffAtMs         = 0;
volatile unsigned long b2500OffAtMs         = 0;

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
String buildUdpResponse(const String& request) {
  const int requestId = extractJsonInt(request, "id");
  const String method = extractJsonString(request, "method");

  // The battery only needs the JSON response and the requested power values.
  const float total   = static_cast<float>(f_currentPowerWatts);

  // Next sample for UDP in 1 second: let fake samples decline with 
  // alpha 0.7 ... 0.9 until we get fresh value from tibber pulse
  f_currentPowerWatts = static_cast<int>(static_cast<float>(
						f_currentPowerWatts) * decayPowBetweenSamples);

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
  if (packetLen <= 0) {
    return;
  }

  char packet[256];
  int len = udp.read(packet, sizeof(packet) - 1);
  if (len <= 0) {
    return;
  }
  packet[len] = '\0';

  String request(packet);
  String response = buildUdpResponse(request);
  if (response.length() == 0) {
    return;
  }

  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.printf("%s", response.c_str());
  udp.endPacket();

  b2500OffAtMs = millis() + 250; // visualize UDP response send by flashing "M"

  //Serial.print("[MarstekTibber] UDP response on port ");
  //Serial.print(port);
  //Serial.print(" for ");
  //Serial.println(request.substring(0, min((int)request.length(), 120)));
}


// -----------------------------------------------------------------------------
// Reading meter data from Pulse via Tibber Bridge
// -----------------------------------------------------------------------------
void tibber_polling_task(void *parameter) {
  HTTPClient http;
  Serial.println("Starting SML-Parser...");
  
  // Einmaliges, fragmentsicheres Berechnen der Tibber-Verbindungsdaten vor dem Loop-Start
  const String url = "http://" + String(tibber_bridge_ip) + "/data.json?node_id=" + String(tibber_node_id);
  const String auth_string = "admin:" + String(tibber_bridge_password);
  const String auth_base64 = "Basic " + base64::encode((uint8_t*)auth_string.c_str(), auth_string.length());

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      http.begin(url);
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
            
            if (valType == 0x53)
              raw_power  = (int16_t)((buffer[valIdx + 0] << 8) | 
                                      buffer[valIdx + 1]);
            else if (valType == 0x54) {
              uint32_t val24   = ((buffer[valIdx + 0] << 16) | 
                                  (buffer[valIdx + 1] <<  8) |
                                   buffer[valIdx + 2]);
              if (val24 & 0x800000) val24 |= 0xFF000000;
              raw_power        = (int32_t)val24;
            }
            else if (valType == 0x55) {
              raw_power        = ((buffer[valIdx + 0] << 24) | 
                                  (buffer[valIdx + 1] << 16) | 
                                  (buffer[valIdx + 2] <<  8) |
                                   buffer[valIdx + 3]);
            }

            g_currentPowerWatts   = (int)(static_cast<float>(raw_power) * multiplier);
            f_currentPowerWatts   = g_currentPowerWatts;
            g_lastPowerUpdateMs   = millis();
			
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
        vTaskDelay(pdMS_TO_TICKS(6000));
      }
	  // guaranteed closure of the socket through whatever path we get here
	  http.end();
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
  server.send(200, "text/plain; charset=utf-8", 
              String(kDeviceId) + " Shelly Pro 3EM emulator\n");
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

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  setCpuFrequencyMhz(80); 

  setup_display(tibber_bridge_ip);
  Serial.println("\n[MarstekTibber] Starting Shelly Pro 3EM emulation");

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

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
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

  vTaskDelay(pdMS_TO_TICKS(1));
}

