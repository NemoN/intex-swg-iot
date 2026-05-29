This is a fork of https://github.com/jressel01/intex-swg-iot
- fixed the build for newer ESP IDF versions up to 4.x (5.x will not work)
- removed Home Assistant NodeRED + MQTT workflow
- created new HomeAssistant Plugin at https://github.com/NemoN/ha-intex-swg (HACS)
- ported the firmware to ESP-IDF 6.x (branch `esp-idf-6.x`, see below)

## What's new on the `esp-idf-6.x` branch

- **ESP-IDF 6.0.1 support** while keeping the cycle-accurate Core1 bit-banging of the SWG/display bus working (requires the CPU at 240 MHz).
- **Reliable API under load**: API commands (power on/off/standby, self-clean) are now placed on a command queue and applied one at a time. Multiple/parallel requests (e.g. several Home Assistant polls) can no longer overwrite an in-flight virtual key press.
- **HTTP server tuned for multiple clients**: more concurrent sockets, stale-connection purging and receive/send timeouts.
- **Display features**: the IP address scrolls across the display once after the WiFi connection is established, and a short startup animation is shown on boot.
- **Service-LED feedback**: the service LED briefly blinks whenever an API request is received.
- **Smaller firmware**: removed unused embedded assets (jQuery, favicon, certificate bundle), freeing flash space.
- **Hardening**: safer debug handler (no large stack buffer), bounded string formatting and compact JSON responses.

# Build notes

> The instructions below are for the original **ESP-IDF 4.x** branch. For the
> ESP-IDF 6.x build, use IDF `v6.0.1` and check out the `esp-idf-6.x` branch
> instead (see [Build notes (ESP-IDF 6.x)](#build-notes-esp-idf-6x)).

ESP IDF Version: https://github.com/espressif/esp-idf/releases/tag/v4.4.8
```
git clone -b v4.4.8 --recursive https://github.com/espressif/esp-idf.git esp-idf-v4.4.8
cd esp-idf-v4.4.8/
./install.sh esp32
. ./export.sh
```

```
git clone https://github.com/NemoN/intex-swg-iot.git
cd intex-swg-iot/
git checkout esp-idf-4.x
idf.py build
idf.py flash
```

# Build notes (ESP-IDF 6.x)

ESP IDF Version: https://github.com/espressif/esp-idf/releases/tag/v6.0.1
```
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.0.1
cd esp-idf-v6.0.1/
./install.sh esp32
. ./export.sh
```

```
git clone https://github.com/NemoN/intex-swg-iot.git
cd intex-swg-iot/
git checkout esp-idf-6.x
idf.py build
idf.py flash
```

> Note: the CPU must run at 240 MHz — the Core1 bus timing depends on it.

# Intex Salt Water Chlorine Generators (SWG) 

You need SWG with 16Pin (TM1650) Chip on the Display Board. It will not work with the 18 Pin Pic. We work on it

For this project, original cable between display board and main board will now be from display board to ESP32 board. You'll need a new cable to connect ESP32 board to the main board. This way the ESP32 will be in the middle of every comunication. For the new cable I used a NEMA17 motor cable (the one that 3D printers use) as I had several ones at home, and one connector fits what I need, so only have to change the other connector from this cable.

## For the circuit you'll need:

- ESP32
- Relay module
- Power supply, I used 5V, but you can use 3.3V instead (wiring is different!)
- 2x Female and 1x Male XH2.54 4pin connectors (they are the most similar I've found that fits original cable)
- Level shifter as the logic from ESP32 is 3.3V and the SWG logic is 5V.
- Fuse for the power supply (optional)
- NEMA17 cable (I used this because I had several ones at home, but you can use whatever you have or you can buy)

## For the wiring, take into account the ESP32 pins:

- GPIO 19 -> SWG main board clock
- GPIO 18 -> SWG main board data
- GPIO 17 -> Display board clock
- GPIO 16 -> Display board data
- GPIO 2 -> Relay module control

PCB Layout https://github.com/jingsno/intex-swg-pcb-TM1650 (Change Relay pin from GPIO0 to GPIO2)

## RestAPI

For the API to control the system there are some endpoints that you could check in the code.
Basic API calls are the following:
- Control the machine
POST http://ip_addr:8080/api/v1/intex/swg
{
"data": {
"power": "{on|off|standby}"
}
}

- Reboot ESP
POST http://ip_addr:8080/api/v1/intex/swg/reboot
{
"data": {
"reboot": "yes"
}
}

- selfclean
POST http://ip_addr:8080/api/v1/intex/swg/self_clean
{
"data": {
"time": "{6|10|12}"
}
}
not testet yet

- Set display brightness (0-7)
POST http://ip_addr:8080/api/v1/intex/swg/display
{
"data": {
"brightness": 4
}
}

- Remove stored WiFi configuration (forces AP setup mode on next boot)
DELETE http://ip_addr:8080/api/v1/intex/swg/wifi

- Get current status
GET http://ip_addr:8080/api/v1/intex/swg/status

- Get current debug
GET http://ip_addr:8080/api/v1/intex/swg/debug

> Control commands (power/self-clean) are queued and applied one at a time, so
> sending several requests in quick succession is safe. The response `status`
> field reports whether the command was accepted into the queue.

## OTA Update

- Enable OTA
POST http://ip_addr:8080/api/v1/intex/swg/enableota
{
"data": {
"enableota": "yes"
}
}

http://ip_addr:8080

## Home Assistant integration.

NodeRed create the sensor and switches and send the Data from Restapi to MQTT

copy swg folder from www to have the Pictures
Add nodered flow and change the ip for the SWG_ESP and take your Homeassistant and MQTT connection

1. Inject "Create Sensor Switch"
2. Inject "Activate MQTT sensor"

Home Assistant have now a new MQTT Device.

Now NodeRed ask every 30sec the esp


Take the content from the yaml to a new Manuell element.

![Dashboard integration](https://github.com/jressel01/intex-swg-iot/blob/919abdecc5d5e0c60420e988dabc0ae782009515/Home%20Assistant%20integration/Dashboard%20Sample.JPG)
