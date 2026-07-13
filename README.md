# MarstekTibber
This is a Shelly Pro EM-50 emulator for Tibber Pulse to connect to a Marstek B2500-D Battery.

This comes in form of an ESP32 Arduino project, which has been created for a Lilygo T-Display or TTGO T-Display board.

<img width="2043" height="1351" alt="TTGO_export" src="https://github.com/user-attachments/assets/115fcbca-7758-4741-95cb-a28e07b5154e" />

## Configuration
1. Add your WiFi SSID and password
2. Find out the IP address of your Tibber Bridge and add the Bridge password
3. Confirm the correct node ID running your Tibber Bridge by accessing its local web interface
4. In the Marstek app, tap on discharge configuration, automatic and select the Shelly PRO EM-50 device. You should see on phase A and Total the actual power value received from the Tibber Pulse/Bridge

## Goals & Advantages
- small power footprint
- cheap hardware
- simple, minimalistic project
- easy to install

## Tibber
In case you intend to subcribe to Tibber as your energy provider anyway and you want to do me a favour, you could consider to use the following invitation code: mvr2g715
