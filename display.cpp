// Display routines for the AstraTibber project

#include <TFT_eSPI.h> 
#include "display.h"

TFT_eSPI               tft                  = TFT_eSPI();

// Ring buffer for power data graph
int g_powerHistory[kGraphMaxSamples]        = {0};
int g_historyCount                          = 0;
int g_historyIndex                          = 0;         // write pointer

// external variables owned by AstraTibber.ino
extern volatile unsigned long pulseOffAtMs;
extern volatile unsigned long b2500OffAtMs;
extern volatile bool          tibberConnected;
extern volatile bool          triggerScreenRefresh;
extern volatile int           g_currentPowerWatts;


// -----------------------------------------------------------------------------
// TTGO / Lilygo T-Display visualization of meter data and basic comm events
// -----------------------------------------------------------------------------
void drawGraph() {
    if (g_historyCount == 0) return;

	// Snap-shot the index so it doesn't change mid-render
    int currentIdx = g_historyIndex;
	
    // Min/Max search
    int minVal = g_powerHistory[0];
    int maxVal = g_powerHistory[0];
    for (int i = 1; i < g_historyCount; i++) {
        if (g_powerHistory[i] < minVal) minVal = g_powerHistory[i];
        if (g_powerHistory[i] > maxVal) maxVal = g_powerHistory[i];
    }

    // avoid div by zero
    if (maxVal == minVal) {
        maxVal++;
        minVal--;
    }

    // zero line if 0 watts within visible range
    if (minVal <= 0 && maxVal >= 0) {
        int zeroY = kGraphY + kGraphHeight - (int)(((0 - minVal) * kGraphHeight) / (maxVal - minVal));
        tft.drawFastHLine(kGraphX, zeroY, kGraphWidth, 0x5A4B); // dark gray
    }

    // Min/Max scale labeling at right side
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(kGraphX + kGraphWidth + 4, kGraphY);
    tft.printf("%dW   ", maxVal);
    tft.setCursor(kGraphX + kGraphWidth + 4, kGraphY + kGraphHeight - 8);
    tft.printf("%dW   ", minVal);

    // Draw data points
    for (int i = 0; i < g_historyCount; i++) {
        // chronological from ring buffer
        int dataIdx = (currentIdx - g_historyCount + i + kGraphMaxSamples) % kGraphMaxSamples;
        int val     = g_powerHistory[dataIdx];

        int x = kGraphX + i;
        int y = kGraphY + kGraphHeight - (int)(((val - minVal) * kGraphHeight) / (maxVal - minVal));

        // Red: Import, Green: Export
        uint16_t color = (val >= 0) ? TFT_RED : TFT_GREEN;

        tft.drawPixel(x, y, color);
    }
}

volatile bool                 T_drawn       = false;
volatile bool                 B_drawn       = false;
volatile bool                 M_drawn       = false;

void updateActivityIndicators() {
  unsigned long now = millis();
  tft.setTextSize(3);

  // "T" for Tibber bridge response
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

  // "M" for answered Marstek UDP broadcast
  if (now < b2500OffAtMs && !M_drawn) {
    tft.setTextColor(COLOR_B2500);
    tft.drawString("M", M_XPos, M_YPos);
	M_drawn = true;
  } else {
	if (M_drawn) {
      tft.fillRect(M_XPos, M_YPos, 20, 25, TFT_BLACK);
	  M_drawn = false;
	}
  }
}

void display_management_task(void *parameter) {
  for (;;) {
    if (triggerScreenRefresh) {
      triggerScreenRefresh = false;
      tft.fillScreen(TFT_BLACK);
	  T_drawn = false;
      B_drawn = false;
      M_drawn = false; 
	  
      tft.setTextSize(3);
	  
      // visualize Tibber bridge status if lost
      if (!tibberConnected) {
        tft.setTextColor(TFT_YELLOW);
        tft.drawString("B?", T_XPos, T_YPos);
		B_drawn = true;
      }
	  
      // Utility grid meter status
      if (g_currentPowerWatts >= 0) {
        tft.setTextColor(TFT_RED);
        tft.drawString("Import", 10, 10);
      } else {
        tft.setTextColor(TFT_GREEN);
        tft.drawString("Export", 10, 10);
      }
      tft.setTextColor(TFT_WHITE);
      String wattStr = String((int)g_currentPowerWatts);
      tft.drawString(wattStr, 10, 50);
      int xPos = 10 + (wattStr.length() * 25);
      tft.drawString("W", xPos + 10, 50);
	  
	  drawGraph();
    }
    
    updateActivityIndicators();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup_display(const char *tibber_bridge_ip) {
  String ipLine = "Bridge: 000.000.000.000";

  tft.init();
  tft.setRotation(3); 
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Connecting...", 10, 10);
  ipLine = String("Bridge: ") + tibber_bridge_ip;
  tft.drawString(ipLine, 10, 30);
}

void ip_info_display(IPAddress localIP) {
  String ipLine = "ESP:    000.000.000.000";

  ipLine = "ESP:    " + localIP.toString();
  tft.drawString(ipLine, 10, 50);
}

