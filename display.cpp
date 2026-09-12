// Display routines for the MarstekTibber project

#include <TFT_eSPI.h> 
#include "display.h"
#include "prices.h"

TFT_eSPI               tft                  = TFT_eSPI();

// Ring buffer for power data graph
int g_powerHistory[kGraphMaxSamples]        = {0};
int g_historyCount                          = 0;
int g_historyIndex                          = 0;         // write pointer

// external variables
extern volatile unsigned long pulseOffAtMs;
extern volatile unsigned long b2500OffAtMs;
extern volatile bool          tibberConnected;
extern volatile bool          triggerScreenRefresh;
extern volatile int           g_currentPowerWatts;

// NEU: Steuert, ob initial die Preiskurve gezeichnet wird
volatile bool                 g_showInitialPrices = true;
extern unsigned long          g_priceScreenTriggerMs;

// -----------------------------------------------------------------------------
// NEU: Zeichnet die geladene Tibber-Preiskurve auf dem kompletten Bildschirm
// -----------------------------------------------------------------------------
void drawPriceGraph() {
    if (g_activeIntervalsCount == 0) {
        tft.setTextSize(2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Warte auf Preise...", 10, 80);
        return;
    }

    // 1. Min/Max Preise zur dynamischen vertikalen Skalierung suchen
    float minPrice = g_priceIntervals[0].price;
    float maxPrice = g_priceIntervals[0].price;
    for (int i = 1; i < g_activeIntervalsCount; i++) {
        if (g_priceIntervals[i].price < minPrice) minPrice = g_priceIntervals[i].price;
        if (g_priceIntervals[i].price > maxPrice) maxPrice = g_priceIntervals[i].price;
    }
    if (maxPrice == minPrice) { maxPrice += 0.05f; minPrice -= 0.05f; }

    // Grafik-Bereich festlegen (Nutzt fast das ganze Display unter der Kopfzeile)
    int pGraphX = 10;
    int pGraphY = 40;
    int pGraphW = 200; // Breite für die Darstellung
    int pGraphH = 80;  // Höhe des Diagramms

    // Skalenbeschriftung rechts
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(pGraphX + pGraphW + 5, pGraphY);
    tft.printf("%.2fc", maxPrice * 100.0f);
    tft.setCursor(pGraphX + pGraphW + 5, pGraphY + pGraphH - 8);
    tft.printf("%.2fc", minPrice * 100.0f);

    // Nulllinie zeichnen (falls Preise negativ werden)
    if (minPrice <= 0.0f && maxPrice >= 0.0f) {
        int zeroY = pGraphY + pGraphH - (int)(((0.0f - minPrice) * pGraphH) / (maxPrice - minPrice));
        tft.drawFastHLine(pGraphX, zeroY, pGraphW, TFT_DARKGREY);
    }

    // 2. Preis-Intervalle auf die Pixelbreite (pGraphW) verteilen und zeichnen
    for (int i = 0; i < pGraphW; i++) {
        // Pixel-Index auf Array-Index abbilden
        int arrayIdx = (i * g_activeIntervalsCount) / pGraphW;
        if (arrayIdx >= g_activeIntervalsCount) arrayIdx = g_activeIntervalsCount - 1;

        float val = g_priceIntervals[arrayIdx].price;
        bool isPeak = g_priceIntervals[arrayIdx].isExpensive;

        // X- und Y-Koordinate berechnen
        int x = pGraphX + i;
        int y = pGraphY + pGraphH - (int)(((val - minPrice) * pGraphH) / (maxPrice - minPrice));

        // VORGABE: Normale Kurve in GRÜN, Preisspitzen (Peaks) in ROT
        uint16_t color = isPeak ? TFT_RED : TFT_GREEN;

        // Als vertikale Stufe zeichnen für saubere Stufenoptik (15-Min-Intervalle)
        tft.drawPixel(x, y, color);
        // Optional: Füllt nach unten hin leicht auf, damit es lesbarer wird
        tft.drawPixel(x, y + 1, color); 
    }
    // reset the 15 s price screen timer once after booting to the current time
    if (g_priceScreenTriggerMs == 1) {
        g_priceScreenTriggerMs = millis();
    }
}

void drawGraph() {
    if (g_historyCount == 0) return;
    int currentIdx = g_historyIndex;
	
    int minVal = g_powerHistory[0];
    int maxVal = g_powerHistory[0];
    for (int i = 1; i < g_historyCount; i++) {
        if (g_powerHistory[i] < minVal) minVal = g_powerHistory[i];
        if (g_powerHistory[i] > maxVal) maxVal = g_powerHistory[i];
    }
    if (maxVal == minVal) { maxVal++; minVal--; }

    if (minVal <= 0 && maxVal >= 0) {
        int zeroY = kGraphY + kGraphHeight - (int)(((0 - minVal) * kGraphHeight) / (maxVal - minVal));
        tft.drawFastHLine(kGraphX, zeroY, kGraphWidth, 0x5A4B); 
    }

    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(kGraphX + kGraphWidth + 4, kGraphY);
    tft.printf("%dW   ", maxVal);
    tft.setCursor(kGraphX + kGraphWidth + 4, kGraphY + kGraphHeight - 8);
    tft.printf("%dW   ", minVal);

    for (int i = 0; i < g_historyCount; i++) {
        int dataIdx = (currentIdx - g_historyCount + i + kGraphMaxSamples) % kGraphMaxSamples;
        int val     = g_powerHistory[dataIdx];
        int x = kGraphX + i;
        int y = kGraphY + kGraphHeight - (int)(((val - minVal) * kGraphHeight) / (maxVal - minVal));
        uint16_t color = (val >= 0) ? TFT_RED : TFT_GREEN;
        tft.drawPixel(x, y, color);
    }
}

volatile bool                 T_drawn       = false;
volatile bool                 B_drawn       = false;
volatile bool                 M_drawn[kMaxBatteries] = {false, false, false, false};

void updateActivityIndicators() {
  unsigned long now = millis();
  tft.setTextSize(3);

  if (now < pulseOffAtMs && !T_drawn) {
    tft.setTextColor(COLOR_PULSE);
    tft.drawString("T", T_XPos, T_YPos);
    T_drawn = true;
  } else {
    if (T_drawn || B_drawn) {
      if(tibberConnected) {
        tft.fillRect(T_XPos, T_YPos, 40, 25, TFT_BLACK);
        T_drawn = false;
        B_drawn = false;
      }
    }
  }

  int spacingX = 22; 
  for (int i = 0; i < g_registeredBatteriesCount; i++) {
      int currentM_XPos = M_XPos + (i * spacingX);
      if (now < g_batteryTimers[i] && !M_drawn[i]) {
          tft.setTextColor(g_batteryColors[i]);
          tft.drawString("M", currentM_XPos, M_YPos);
          M_drawn[i] = true;
      } else if (now >= g_batteryTimers[i] && M_drawn[i]) {
          tft.fillRect(currentM_XPos, M_YPos, 20, 25, TFT_BLACK);
          M_drawn[i] = false;
      }
  }
}

void display_management_task(void *parameter) {
  for (;;) {
    // 1. Decrement price screen timer (if active and enabled)
    // as long g_priceScreenTriggerMs == 1, wait for loading data
    if (g_showInitialPrices && g_priceScreenTriggerMs > 1) {
      if (millis() - g_priceScreenTriggerMs > 15000) {
        g_showInitialPrices    = false;
        triggerScreenRefresh   = true;
        g_priceScreenTriggerMs = 0;  // Sperren, Countdown beendet.
      }
    }

    // 2. Button press to enter price screen later
    if (!g_showInitialPrices && g_priceScreenTriggerMs > 1) {
        g_showInitialPrices  = true;
        triggerScreenRefresh = true;
    }

    if (triggerScreenRefresh) {
      triggerScreenRefresh = false;
      tft.fillScreen(TFT_BLACK);
      T_drawn = false;
      B_drawn = false;
      for (int i = 0; i < kMaxBatteries; i++) { M_drawn[i] = false; }
	  
      tft.setTextSize(3);
	  
      if (!tibberConnected) {
        tft.setTextColor(TFT_YELLOW);
        tft.drawString("B?", T_XPos, T_YPos);
        B_drawn = true;
      }
	  
      // Switch between price diagram and live consumption
      if (g_showInitialPrices) {
        tft.setTextColor(TFT_WHITE);
        tft.drawString("Price", 10, 10);
        
        // Rufe die neue Preiskurven-Zeichenfunktion auf
        drawPriceGraph();
      } else {
        // Standard Live-Verbrauchsanzeige
        if (g_currentPowerWatts >= 0) {
          tft.setTextColor(TFT_RED);
          tft.drawString("Imp", 10, 10);  // Import
        } else {
          tft.setTextColor(TFT_GREEN);
          tft.drawString("Exp", 10, 10);  // Export
        }

        // Meter data [W]
        tft.setTextColor(TFT_WHITE);
        String wattStr = String((int)g_currentPowerWatts);
        tft.drawString(wattStr, 10, 50);
        int xPos = 6 + (wattStr.length() * 24);
        tft.drawString("W", xPos, 50);

        // Price info
        float currentPrice = getCurrentIntervalPrice();
        if (g_activeIntervalsCount > 0 && currentPrice > 0.0f) {
          tft.setTextSize(2);
          uint16_t priceColor = isCurrentIntervalExpensive() ? TFT_RED : TFT_GREEN;
          tft.setTextColor(priceColor, TFT_BLACK);
          
          int priceXPos = xPos + 30;    // distance behind 'W'
          tft.setCursor(priceXPos, 58); // y correction due to smaller height
          
          tft.printf("%.2fc", currentPrice * 100.0f);
        }
        drawGraph();
      }
    }
    
    updateActivityIndicators();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup_display(const char *tibber_bridge_ip) {
  tft.init();
  tft.setRotation(3); 
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Connecting...", 10, 10);
  String ipLine = String("Bdg:") + tibber_bridge_ip;
  tft.drawString(ipLine, 10, 30);
}

void ip_info_display(IPAddress localIP) {
  String ipLine = "ESP:" + localIP.toString();
  tft.drawString(ipLine, 10, 50);
}

float getCurrentIntervalPrice() {
  time_t now = time(nullptr);
  for (int i = 0; i < g_activeIntervalsCount; i++) {
    if (now >= g_priceIntervals[i].startEpoch && now < (g_priceIntervals[i].startEpoch + 900)) {
      return g_priceIntervals[i].price;
    }
  }
  return 0.0f; // Fallback, falls noch keine Preise geladen wurden
}
