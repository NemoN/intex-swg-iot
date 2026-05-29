# Intex SWG IoT

> ESP32 firmware that sits between the display board and the main board of an
> Intex Salt Water Chlorine Generator (SWG) and exposes a REST API + OTA web
> interface to monitor and control it.

This is a fork of [jressel01/intex-swg-iot](https://github.com/jressel01/intex-swg-iot).

- Removed the Home Assistant NodeRED + MQTT workflow.
- Created a new Home Assistant plugin: [NemoN/ha-intex-swg](https://github.com/NemoN/ha-intex-swg) (HACS).
- Ported the firmware to **ESP-IDF 6.x** (branch [`esp-idf-6.x`](#build-esp-idf-6x)).

## Contents

- [What's new on `esp-idf-6.x`](#whats-new-on-esp-idf-6x)
- [Build (ESP-IDF 6.x)](#build-esp-idf-6x)
- [Hardware](#hardware)
  - [Parts list](#parts-list)
  - [Wiring](#wiring)
- [REST API](#rest-api)
- [OTA update](#ota-update)

## What's new on `esp-idf-6.x`

| Feature | Description |
| --- | --- |
| **ESP-IDF 6.0.1 support** | Keeps the cycle-accurate Core1 bit-banging of the SWG/display bus working (requires the CPU at 240 MHz). |
| **Reliable API under load** | Commands (`power` on/off/standby, `self_clean`) are placed on a queue and applied one at a time, so parallel requests (e.g. several Home Assistant polls) can't overwrite an in-flight virtual key press. |
| **HTTP server tuned for multiple clients** | More concurrent sockets, stale-connection purging and receive/send timeouts. |
| **Display features** | The IP address scrolls across the display once after WiFi connects, plus a short startup animation on boot. |
| **Service-LED feedback** | The service LED briefly blinks whenever an API request is received. |
| **Smaller firmware** | Removed unused embedded assets (jQuery, favicon, certificate bundle) to free flash space. |
| **Hardening** | Safer debug handler (no large stack buffer), bounded string formatting and compact JSON responses. |

## Build (ESP-IDF 6.x)

Install [ESP-IDF v6.0.1](https://github.com/espressif/esp-idf/releases/tag/v6.0.1):

```bash
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.0.1
cd esp-idf-v6.0.1/
./install.sh esp32
. ./export.sh
```

Build and flash the firmware:

```bash
git clone https://github.com/NemoN/intex-swg-iot.git
cd intex-swg-iot/
git checkout esp-idf-6.x
idf.py build
idf.py flash
```

> [!IMPORTANT]
> The CPU must run at **240 MHz** — the Core1 bus timing depends on it.

## Hardware

You need an SWG with the **16-pin (TM1650)** chip on the display board. It will
**not** work with the 18-pin PIC (work in progress).

The original cable between the display board and the main board now goes from the
display board to the ESP32 board. You'll need a new cable to connect the ESP32
board to the main board, so the ESP32 sits in the middle of every communication.
A NEMA17 motor cable (the kind used by 3D printers) works well since one
connector already fits; only the other connector needs swapping.

### Parts list

- ESP32
- Relay module
- Power supply — 5 V (or 3.3 V, but the wiring differs!)
- 2x female and 1x male XH2.54 4-pin connectors (closest match to the original cable)
- Level shifter (ESP32 logic is 3.3 V, the SWG logic is 5 V)
- Fuse for the power supply (optional)
- NEMA17 cable (or any cable you have/can buy)

### Wiring

| ESP32 GPIO | Connects to |
| --- | --- |
| GPIO 19 | SWG main board clock |
| GPIO 18 | SWG main board data |
| GPIO 17 | Display board clock |
| GPIO 16 | Display board data |
| GPIO 2  | Relay module control |

PCB layout: [jingsno/intex-swg-pcb-TM1650](https://github.com/jingsno/intex-swg-pcb-TM1650)
(change the relay pin from GPIO0 to GPIO2).

## REST API

The firmware serves the API on port **8080**. Replace `<ip_addr>` with the
ESP32's IP address.

| Method | Endpoint | Description |
| --- | --- | --- |
| `POST` | `/api/v1/intex/swg` | Control the machine (power) |
| `POST` | `/api/v1/intex/swg/self_clean` | Start self-clean cycle |
| `POST` | `/api/v1/intex/swg/display` | Set display brightness |
| `POST` | `/api/v1/intex/swg/reboot` | Reboot the ESP32 |
| `DELETE` | `/api/v1/intex/swg/wifi` | Remove stored WiFi config |
| `GET` | `/api/v1/intex/swg/status` | Current status |
| `GET` | `/api/v1/intex/swg/debug` | Debug information |

> [!NOTE]
> Control commands (`power` / `self_clean`) are queued and applied one at a
> time, so sending several requests in quick succession is safe. The response
> `status` field reports whether the command was accepted into the queue.

### Control the machine

```http
POST http://<ip_addr>:8080/api/v1/intex/swg
Content-Type: application/json
```

```json
{
  "data": {
    "power": "on"
  }
}
```

`power` accepts `on`, `off` or `standby`.

### Self-clean

Sets the daily operating time in **hours** (`6`, `10` or `14`).

```http
POST http://<ip_addr>:8080/api/v1/intex/swg/self_clean
Content-Type: application/json
```

```json
{
  "data": {
    "time": 6
  }
}
```

> [!WARNING]
> Self-clean is not fully tested yet.

### Set display brightness

Brightness ranges from `0` to `7`.

```http
POST http://<ip_addr>:8080/api/v1/intex/swg/display
Content-Type: application/json
```

```json
{
  "data": {
    "brightness": 4
  }
}
```

### Reboot the ESP32

```http
POST http://<ip_addr>:8080/api/v1/intex/swg/reboot
Content-Type: application/json
```

```json
{
  "data": {
    "reboot": "yes"
  }
}
```

### Remove stored WiFi configuration

Forces AP setup mode on the next boot.

```http
DELETE http://<ip_addr>:8080/api/v1/intex/swg/wifi
```

### Get status / debug

```http
GET http://<ip_addr>:8080/api/v1/intex/swg/status
GET http://<ip_addr>:8080/api/v1/intex/swg/debug
```

## OTA update

1. Enable OTA:

   ```http
   POST http://<ip_addr>:8080/api/v1/intex/swg/enableota
   Content-Type: application/json
   ```

   ```json
   {
     "data": {
       "enableota": "yes"
     }
   }
   ```

2. Open the OTA web interface in a browser and upload the new firmware:

   ```
   http://<ip_addr>:8080
   ```
