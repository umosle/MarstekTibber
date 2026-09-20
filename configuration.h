// -----------------------------------------------------------------------------
// User specific Configuration
// -----------------------------------------------------------------------------
// Replace these with your own values before flashing the device.
constexpr char      kWifiSsid[]             = "WiFi_SSID";
constexpr char      kWifiPassword[]         = "WiFi_password";
//constexpr char    kDeviceId[]             = "shellypro3em-ec4609c439c1";
constexpr char      kDeviceId[]             = "shellyproem50-ec4609c439c2";

const char*         tibber_bridge_ip        = "192.168.178.**";     // your local IP of the Tibber bridge
const char*         tibber_bridge_password  = "B***-****";          // code beside the QR
const int           tibber_node_id          = 1;                    // open your bridge web-server to check for Pulse node ID
const unsigned long tibberIntervalMs        = 4000;                 // miliseconds, shorther queries might stress the bridge
const char*         TIBBER_ACCESS_TOKEN     = "5**************************************************************34-1";
const char*         TIBBER_HOME_ID          = "c*******-****-****-****-**********0c";

// 1. if tibber connection lost decay power slowly over 5 minutes
// 2. in between samples a slight decay might help against nervouse polling within
//    dead time of tibber pulse
const float         decayPowBetweenSamples  = 0.70f; // adjust to avoid oscillation
const int           BUTTON_PIN              = 0;     // Button for price screen (35-bottom or 0-top, USB left)
