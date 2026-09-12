#include "prices.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <vector> // for dynamic memory use

// Globale Variablen Definitionen
PriceInterval g_priceIntervals[kTotalIntervals];
int g_activeIntervalsCount = 0;
float g_currentThreshold   = 99.0f;

static unsigned long lastFetchMillis  = 0;
static bool          initialFetchDone = false;

// Hilfsfunktion: Konvertiert ISO8601 (2026-09-09T14:15:00.000+02:00) rudimentär in Epoch-Sekunden
// Reicht für den reinen Relativ-Vergleich der Intervalle völlig aus
static uint32_t parseISO8601ToEpoch(const String& iso) {
    if (iso.length() < 19) return 0;
    struct tm tm;
    tm.tm_year = iso.substring(0, 4).toInt() - 1900;
    tm.tm_mon  = iso.substring(5, 7).toInt() - 1;
    tm.tm_mday = iso.substring(8, 10).toInt();
    tm.tm_hour = iso.substring(11, 13).toInt();
    tm.tm_min  = iso.substring(14, 16).toInt();
    tm.tm_sec  = iso.substring(17, 19).toInt();
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void initPrices() {
    memset(g_priceIntervals, 0, sizeof(g_priceIntervals));
    g_activeIntervalsCount = 0;
}

// Interne Funktion zum Sortieren von Floats (für Schwellenwert-Ermittlung)
static int compareFloats(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

static void fetchTibberPrices() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin("https://api.tibber.com/v1-beta/gql");
    http.addHeader("Authorization", "Bearer " + String(TIBBER_ACCESS_TOKEN));
    http.addHeader("Content-Type", "application/json");

    // GraphQL Query kompakt formatiert
    String query = "{\"query\": \"{ viewer { home(id: \\\"" + String(TIBBER_HOME_ID) + "\\\") { currentSubscription { priceInfo(resolution: QUARTER_HOURLY) { today { startsAt total } tomorrow { startsAt total } } } } } }\"}";

    Serial.println("[Prices] Rufe 15-Min-Preise von Tibber ab...");
    int httpCode = http.POST(query);

    if (httpCode == 200) {
        String response = http.getString();

        int searchPos = 0;
        // Stream parser to ring buffer
        while (searchPos < response.length()) {
            int startsAtPos = response.indexOf("\"startsAt\":\"", searchPos);
            if (startsAtPos == -1) break;
            startsAtPos += 12;
            int startsAtEnd = response.indexOf("\"", startsAtPos);
            String startsAt = response.substring(startsAtPos, startsAtEnd);

            int totalPos = response.indexOf("\"total\":", startsAtEnd);
            if (totalPos == -1) break;
            totalPos += 8;
            int totalEnd = response.indexOf(",", totalPos);
            if (totalEnd == -1 || totalEnd > response.indexOf("}", totalPos)) {
                totalEnd = response.indexOf("}", totalPos);
            }
            String totalVal = response.substring(totalPos, totalEnd);
            searchPos = totalEnd;

            uint32_t epoch = parseISO8601ToEpoch(startsAt);
            float price = totalVal.toFloat();

            // Prüfen, ob dieses Intervall bereits existiert (Datenrettung!)
            bool exists = false;
            for (int i = 0; i < g_activeIntervalsCount; i++) {
                if (g_priceIntervals[i].startEpoch == epoch) {
                    g_priceIntervals[i].price = price; // Preis aktualisieren falls nötig
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                // Wenn das Array voll ist, schieben wir den ältesten Tag (96 Elemente) raus
                if (g_activeIntervalsCount >= kTotalIntervals) {
                    memmove(&g_priceIntervals[0], &g_priceIntervals[96], (kTotalIntervals - 96) * sizeof(PriceInterval));
                    g_activeIntervalsCount -= 96;
                    Serial.println("[Prices] Ältesten Tag aus dem RAM rotiert.");
                }

                // Am Ende anhängen
                g_priceIntervals[g_activeIntervalsCount].startEpoch = epoch;
                g_priceIntervals[g_activeIntervalsCount].price = price;
                g_priceIntervals[g_activeIntervalsCount].isExpensive = false;
                g_priceIntervals[g_activeIntervalsCount].avgPowerWatts = 0; // Neu, wartet auf Pulse-Logging
                g_activeIntervalsCount++;
            }
        }

        Serial.printf("[Prices] %d Intervalle eingelesen.\n", g_activeIntervalsCount);

        // ==============================================================================
        // Zeitfenster mit Peaks zwischen Heute 14:00 bis Morgen 14:00
        // ==============================================================================
        time_t now = time(nullptr);
        struct tm timeinfo;
        
        // Sichere Kopie der lokalen Zeit erstellen, um Seiteneffekte zu vermeiden
        if (!getLocalTime(&timeinfo)) {
            Serial.println("[Prices] Fehler: Konnte lokale Systemzeit nicht lesen!");
            // Fallback: Nutze den ersten geladenen Wert als Zeit-Anker
            time_t fallbackTime = g_priceIntervals[0].startEpoch;
            struct tm* tm_fb = localtime(&fallbackTime);
            if (tm_fb) timeinfo = *tm_fb;
        }

        // Lokale Arbeitskopie für die Fenster-Berechnung modifizieren
        struct tm t_calc = timeinfo;
        t_calc.tm_hour = 14;
        t_calc.tm_min = 0;
        t_calc.tm_sec = 0;
        
        time_t fenster_start = mktime(&t_calc);

        // WICHTIG: Wenn wir JETZT vor 14:00 Uhr sind, startete das relevante 
        // 24h-Preisfenster bereits GESTERN um 14:00 Uhr!
        if (timeinfo.tm_hour < 14) {
            fenster_start -= 86400; // 1 Tag zurückschalten
        }
        
        time_t fenster_ende = fenster_start + 86400; // Ende ist immer Start + 24h

        // FIX: Nutzen eines sicheren Vektors auf dem Heap anstelle des Stack-Arrays
        std::vector<float> preise_im_fenster;
        preise_im_fenster.reserve(192); // Optimiert für max 48 Stunden á 4 Intervalle

        // Nur die Preise in das Array aufnehmen, die TATSÄCHLICH im Fenster liegen
        for (int i = 0; i < g_activeIntervalsCount; i++) {
            if (g_priceIntervals[i].startEpoch >= fenster_start && g_priceIntervals[i].startEpoch < fenster_ende) {
                preise_im_fenster.push_back(g_priceIntervals[i].price);
            }
        }

        int windowCount = preise_im_fenster.size();

        // Schwellenwert ermitteln
        if (windowCount >= kNumPeaks) {
            // .data() liefert den vom qsort erwarteten void* Zeiger auf das interne Array
            qsort(preise_im_fenster.data(), windowCount, sizeof(float), compareFloats);
            g_currentThreshold = preise_im_fenster[windowCount - kNumPeaks];
        } else if (windowCount > 0) {
            qsort(preise_im_fenster.data(), windowCount, sizeof(float), compareFloats);
            g_currentThreshold = preise_im_fenster[0]; // Fallback auf das Minimum im Fenster
        } else {
            // Letztes Sicherheitsnetz: Wenn das Fenster leer ist, nimm einfach den allerersten verfügbaren Preis als Fallback
            Serial.println("[Prices] Warnung: Zeitfenster leer! Nutze globalen Fallback.");
            if (g_activeIntervalsCount > 0) {
                g_currentThreshold = g_priceIntervals[0].price;
            } else {
                g_currentThreshold = 99.0f; // Absoluter Not-Fallback (Sperrt Batterie-Einspeisung)
            }
        }

        // Die 'isExpensive' Flags exakt setzen
        for (int i = 0; i < g_activeIntervalsCount; i++) {
            if (g_priceIntervals[i].startEpoch >= fenster_start && 
                g_priceIntervals[i].startEpoch < fenster_ende && 
                g_priceIntervals[i].price >= g_currentThreshold) {
                g_priceIntervals[i].isExpensive = true;
            }
        }
        
        // Debug-Ausgaben zur Kontrolle im Serial Monitor
        char startStr[32], endeStr[32]; // Platz für den Datums-String bereitstellen
        strftime(startStr, sizeof(startStr), "%Y-%m-%d %H:%M", localtime(&fenster_start));
        strftime(endeStr, sizeof(endeStr), "%Y-%m-%d %H:%M", localtime(&fenster_ende));

        
        Serial.printf("[Prices] Aktives Fenster: %s bis %s\n", startStr, endeStr);
        Serial.printf("[Prices] Schwellenwert (Top %d aus %d Intervallen): %.4f EUR/kWh\n", kNumPeaks, windowCount, g_currentThreshold);
    } else {
        Serial.printf("[Prices] HTTP-Fehler bei Preisabfrage: %d\n", httpCode);
    }
    http.end();
}

void checkAndFetchPrices() {
    unsigned long currentMillis = millis();
    
    // 1. Direkt nach dem Booten abfragen (sobald WLAN da ist)
    if (!initialFetchDone && WiFi.status() == WL_CONNECTED) {
        // Kurzer interner Zeitsynchronisations-Check (wichtig für time(nullptr))
        configTime(3600, 3600, "pool.ntp.org"); 
        fetchTibberPrices();
        initialFetchDone = true;
        lastFetchMillis = currentMillis;
        return;
    }

    // 2. Zyklische Prüfung (alle 15 Minuten prüfen, ob es 14:00 Uhr ist)
    // Verhindert dauerhaftes Polling der API
    if (currentMillis - lastFetchMillis >= 15 * 60 * 1000) {
        lastFetchMillis = currentMillis;
        
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        
        // Täglich um 14:00 Uhr neu triggern (Stunde 14, Minute zwischen 0 und 15)
        if (timeinfo->tm_hour == 14 && timeinfo->tm_min < 15) {
            fetchTibberPrices();
        }
    }
}

bool isCurrentIntervalExpensive() {
    time_t now = time(nullptr);
    
    for (int i = 0; i < g_activeIntervalsCount; i++) {
        // Liegt die aktuelle Zeit innerhalb dieses 15-Minuten (900 Sek) Intervalls?
        if (now >= g_priceIntervals[i].startEpoch && now < (g_priceIntervals[i].startEpoch + 900)) {
            return g_priceIntervals[i].isExpensive;
        }
    }
    // Fallback falls außerhalb der bekannten Intervalle: Keine Nulleinspeisung erzwingen
    return false;
}

String getPriceIntervalsJson() {
    // Schätzung für Buffer-Größe: ca. 120 Zeichen pro Intervall * max 192 Intervalle
    String json = "";
    // Reserviert 390 KB RAM vorab für das maximale 31-Tage-JSON, um Heap-Overflows zu verhindern
    json.reserve(390000); 
    json += "[\n";

    for (int i = 0; i < g_activeIntervalsCount; i++) {
        time_t epoch = g_priceIntervals[i].startEpoch;
        struct tm* timeinfo = localtime(&epoch);
        char timeBuffer[20];
        strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M", timeinfo);

        json += "  {\n";
        json += "    \"index\": " + String(i) + ",\n";
        json += "    \"time\": \"" + String(timeBuffer) + "\",\n";
        json += "    \"epoch\": " + String(g_priceIntervals[i].startEpoch) + ",\n";
        json += "    \"price\": " + String(g_priceIntervals[i].price, 4) + ",\n";
        json += "    \"isExpensive\": " + String(g_priceIntervals[i].isExpensive ? "true" : "false") + ",\n";
        json += "    \"power\": " + String(g_priceIntervals[i].avgPowerWatts) + "\n";
        json += "  }";
        
        if (i < g_activeIntervalsCount - 1) {
            json += ",\n";
        } else {
            json += "\n";
        }
    }
    
    json += "]";
    return json;
}
