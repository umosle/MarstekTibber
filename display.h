// Display routines for the AstraTibber project

#pragma once                  // don't include twice
#include <Arduino.h> 


// --- Graph configuration for activity indicators T/M ---
#define             COLOR_PULSE              0x37FF  
#define             COLOR_B2500              0x915F

const int           T_XPos                 = 160;
const int           T_YPos                 = 10;
const int           M_XPos                 = 200;
const int           M_YPos                 = 10;

// --- Graph configuration for power meter data ---
constexpr int       kGraphX                = 5;          // Start-X
constexpr int       kGraphY                = 85;         // Start-Y (lower half)
constexpr int       kGraphWidth            = 185;        // Width (220 Samples)
constexpr int       kGraphHeight           = 45;         // Height
constexpr int       kGraphMaxSamples       = kGraphWidth;


// definitions from display.cpp needed in AstraTibber.ino
extern int          g_powerHistory[kGraphMaxSamples];
extern int          g_historyCount;
extern int          g_historyIndex;


// public API for the main sketch
void display_management_task(void *parameter);
void setup_display(const char *tibber_bridge_ip);
void ip_info_display(IPAddress localIP);

