#ifndef PRICES_H
#define PRICES_H

#include <Arduino.h>

// --- Konfiguration ---
constexpr int kNumPeaks = 32;          // Top X teuerste 15-Min-Intervalle für Nulleinspeisung
constexpr int kTotalIntervals = 3072;  // memory for 1 month (31 + 1d)
extern const char*  TIBBER_ACCESS_TOKEN;
extern const char*  TIBBER_HOME_ID;

// Struct für ein einzelnes Preisintervall
struct PriceInterval {
  uint32_t startEpoch;  // Unix-Timestamp (Sekunden seit 1970) des Intervall-Beginns
  float price;          // Preis in EUR/kWh
  bool isExpensive;     // Flag, ob es zu den Top X teuersten gehört
  int32_t avgPowerWatts;
};

// --- Globale Variablen (Deklaration) ---
extern PriceInterval g_priceIntervals[kTotalIntervals];
extern int g_activeIntervalsCount;
extern float g_currentThreshold;

// --- Funktionen ---
void initPrices();
void checkAndFetchPrices();
bool isCurrentIntervalExpensive();

// for HTTP endpoint
String getPriceIntervalsJson();

#endif  // PRICES_H
