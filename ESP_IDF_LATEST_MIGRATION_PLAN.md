# ESP-IDF Latest Migration Plan

Ziel ist die Migration dieses Projekts von ESP-IDF 4.4.x auf eine aktuelle stabile ESP-IDF-Version fuer ESP32, mit Fokus auf ESP-IDF 5.x und neuer. Der groesste Aufwand liegt nicht im CMake-Grundgeruest, sondern in privaten Hardwarezugriffen, Watchdog-Verhalten, alten Build-/Config-Annahmen und einer sauberen Abhaengigkeitsdefinition fuer lokale und externe Komponenten.

## Ausgangslage

- Das Projekt baut aktuell mit ESP-IDF 4.4.x.
- ESP-IDF 5.x funktioniert derzeit nicht ohne Anpassungen.
- Root-CMake und `main/CMakeLists.txt` nutzen bereits das moderne IDF-CMake-Schema.
- Die kritischen Migrationspunkte liegen in `main/IntexSWG.cpp`, `main/utils.h`, `main/utils.cpp`, `main/RestServer.cpp` und der lokalen `esp32-wifi-manager`-Komponente.

## 1. Baseline festlegen

1. Mit der bestehenden ESP-IDF-4.4.x-Umgebung einmal sauber bauen.
2. Build-Warnungen und bekannte Fehler dokumentieren, damit sie spaeter nicht mit echten Migrationsfehlern verwechselt werden.
3. Fuer die neue ESP-IDF-Version aus einem sauberen Build-Verzeichnis starten.
4. Ziel sollte eine aktuelle stabile ESP-IDF-Version fuer ESP32 sein, nicht `master`.

Empfohlene Referenzbefehle:

```sh
idf.py fullclean
idf.py build
```

## 2. Build- und Dependency-Migration

Die Root-Datei `CMakeLists.txt` kann voraussichtlich bleiben:

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components/)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(IntexSWG)
```

Anpassungen:

- `main/CMakeLists.txt` expliziter machen und benoetigte Komponenten deklarieren.
- Wahrscheinliche Abhaengigkeiten: `driver`, `esp_timer`, `esp_wifi`, `esp_netif`, `nvs_flash`, `esp_http_server`, `app_update`, `json` und Flash-Komponenten.
- `components/esp32-wifi-manager/CMakeLists.txt` auf aktuelle IDF-Versionen vereinfachen.
- Alte IDF-3.x-Branches entfernen, sofern keine Rueckwaertskompatibilitaet zu IDF 3.x gewuenscht ist.
- Legacy-Makefiles aus dem aktiven Pfad entfernen oder klar als obsolete markieren:
  - `components/esp32-wifi-manager/component.mk`
  - `components/esp32-wifi-manager/src/component.mk`
- Fuer aktuelle ESP-IDF-Versionen pruefen, ob `mdns` ueber den Component Manager eingebunden werden muss.
- Falls noetig, ein `idf_component.yml` fuer Registry-Abhaengigkeiten wie `espressif/mdns` ergaenzen.

## 3. sdkconfig neu erzeugen

Die bestehende `sdkconfig` sollte nicht blind uebernommen werden. Sie enthaelt 4.4.x-spezifische Optionen und kann mit aktueller ESP-IDF fehlerhafte oder veraltete Kconfig-Werte enthalten.

Vorgehen:

```sh
idf.py set-target esp32
idf.py reconfigure
idf.py menuconfig
```

Zu pruefen und zu erhalten:

- Custom Partition Table `partitions.csv`
- FreeRTOS Tick Rate, aktuell `CONFIG_FREERTOS_HZ=1000`
- Task-Watchdog-Einstellungen
- WiFi-Manager-Kconfig
- mDNS-Einstellungen
- HTTP-Server-Limits und Stack-Groessen

Nicht uebernehmen:

- alte TCP/IP-Adapter-Compatibility-Optionen
- veraltete System-Event-Compatibility-Optionen
- automatisch migrierte Optionen, die ESP-IDF 5.x nicht mehr kennt

## 4. Entfernte Flash-API ersetzen

Datei: `main/IntexSWG.cpp`

Aktuell kritisch:

```cpp
#include "esp_spi_flash.h"
spi_flash_get_chip_size()
```

ESP-IDF 5.x entfernt beziehungsweise ersetzt diese alte API. Migration:

```cpp
#include "esp_flash.h"

uint32_t flash_size = 0;
esp_err_t flash_result = esp_flash_get_size(esp_flash_default_chip, &flash_size);
```

Danach die Boot-Ausgabe so umbauen, dass ein Fehler beim Lesen der Flash-Groesse sauber geloggt wird.

## 5. Private SOC- und Watchdog-Register entfernen

Datei: `main/IntexSWG.cpp`

Aktuell kritisch:

```cpp
#include "soc/rtc_wdt.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
TIMERG0.wdt_feed = 1;
TIMERG1.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
TIMERG1.wdt_feed = 1;
```

Diese direkten Registerzugriffe sind nicht stabil ueber ESP-IDF-Versionen hinweg. Migration:

- private SOC-/TimerGroup-Includes entfernen
- `feedTheDog()` ueber oeffentliche Watchdog-APIs ersetzen
- Core1-Task korrekt beim Task-Watchdog registrieren, falls sie ueberwacht werden soll
- WDT-Konfiguration bewusst auf die Task-Architektur abstimmen

Moeglicher Zielpfad:

```cpp
#include "esp_task_wdt.h"

void feedTheDog() {
    esp_task_wdt_reset();
}
```

Wichtig: Die aktuelle Core1-Schleife deaktiviert Interrupts sehr lange. Das ist ein hohes Risiko fuer aktuelle ESP-IDF-Versionen. Im ersten Schritt kann die bestehende Architektur beibehalten werden, aber nach der API-Migration muss das Verhalten auf echter Hardware getestet werden.

## 6. GPIO-Abstraktion refaktorieren

Datei: `main/utils.h`

Aktuell kritisch:

```cpp
#define GPIO_Set(x)      REG_WRITE(GPIO_OUT_W1TS_REG, 1<<x)
#define GPIO_Clear(x)    REG_WRITE(GPIO_OUT_W1TC_REG, 1<<x)
#define GPIO_IN_Get(x)   (REG_READ(GPIO_IN_REG) & (1<<x))>>x
```

Diese Makros umgehen den GPIO-Treiber und greifen direkt auf Register zu. Migration:

- `REG_WRITE`/`REG_READ`-Makros entfernen
- typisierte Wrapper ueber `gpio_set_level()` und `gpio_get_level()` einfuehren
- `gpio_num_t` statt untypisierter Pin-Werte verwenden
- `digitalRead()` muss einen logischen Level zurueckgeben, nicht eine Bitmaske

Moeglicher Zielpfad:

```cpp
static inline void GPIO_Set(gpio_num_t pin) {
    gpio_set_level(pin, 1);
}

static inline void GPIO_Clear(gpio_num_t pin) {
    gpio_set_level(pin, 0);
}

static inline uint32_t GPIO_IN_Get(gpio_num_t pin) {
    return gpio_get_level(pin);
}
```

Betroffene Dateien:

- `main/IntexSWG.cpp`
- `main/TM1650.cpp`
- `main/utils.cpp`
- `main/utils.h`

Nach diesem Schritt ist ein Timingtest mit Logikanalysator oder Oszilloskop wichtig. Wenn die GPIO-Treiberaufrufe fuer den SWG-/TM1650-Bus zu langsam sind, sollte erst dann gezielt ueber eine versionierte Low-Level-Alternative nachgedacht werden.

## 7. Zeit- und Delay-APIs bereinigen

Datei: `main/utils.cpp`

Anpassungen:

- `esp_timer_get_time()` explizit ueber `esp_timer.h` einbinden.
- `esp_event.h` aus `utils.cpp` entfernen, falls nur fuer Timer-Funktionen eingebunden.
- `delayMicroseconds()` entweder bewusst als Busy-Wait behalten oder auf `esp_rom_delay_us()` umstellen.
- Rohes `vTaskDelay(1000)`, `vTaskDelay(10)` und `vTaskDelay(... / portTICK_PERIOD_MS)` standardisieren.

Ziel:

```cpp
vTaskDelay(pdMS_TO_TICKS(1000));
```

Betroffene Dateien:

- `main/IntexSWG.cpp`
- `main/RestServer.cpp`

## 8. OTA-Pfad robuster machen

Datei: `main/RestServer.cpp`

Aktuell sollte der OTA-Pfad fuer aktuelle IDF-Versionen robuster werden:

- `esp_ota_get_next_update_partition(NULL)` auf `NULL` pruefen.
- Ergebnis von `strstr(ota_buff, "\r\n\r\n")` pruefen, bevor `+ 4` gerechnet wird.
- Return-Werte von `esp_ota_begin()`, `esp_ota_write()`, `esp_ota_end()` und `esp_ota_set_boot_partition()` konsequent behandeln.
- Bei OTA-Fehlern `esp_ota_abort()` verwenden.
- HTTP-Fehlerantworten zurueckgeben, wenn Upload oder Flashen fehlschlaegt.
- Bei grossen Uploads Watchdog-/Yield-Verhalten pruefen.

## 9. HTTP-/REST-Server und Logging nachziehen

Datei: `main/RestServer.cpp`

Anpassungen:

- HTTPD `stack_size` von `8192` pruefen; fuer IDF 5.x eher `12288` oder `16384` testen.
- Doppelte `config.lru_purge_enable = true;`-Zuweisung entfernen.
- OTA- und REST-Handler mit sauberem Error-Logging versehen.

Datei: `main/IntexSWG.cpp`

Anpassungen:

- `printf()`-Ausgaben schrittweise auf `ESP_LOGI()`/`ESP_LOGW()` umstellen.
- Formatstrings fuer `uint32_t`, `size_t` und ESP-IDF-Typen pruefen.
- Doppelte oder unbenutzte Includes entfernen.

## 10. WiFi-Manager-Komponente pruefen

Dateien:

- `components/esp32-wifi-manager/CMakeLists.txt`
- `components/esp32-wifi-manager/src/wifi_manager.c`
- `components/esp32-wifi-manager/src/http_app.c`

Positiv:

- Die Komponente nutzt bereits `esp_netif`.
- Die Komponente nutzt bereits `esp_event_handler_instance_register()`.

Zu pruefen:

- explizite CMake-Abhaengigkeiten fuer `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, `esp_http_server`, `lwip` und `mdns`
- mDNS-Verfuegbarkeit in aktueller ESP-IDF-Umgebung
- Captive Portal
- gespeicherte STA-Verbindung
- Reconnect nach `WIFI_REASON_BEACON_TIMEOUT`
- Callbacks `WM_EVENT_STA_GOT_IP` und `WM_EVENT_STA_DISCONNECTED`

## 11. Optionales Architektur-Refactoring fuer Core1

Der bestehende Core1-Code in `main/IntexSWG.cpp` ist timingkritisch und deaktiviert Interrupts lange. Das sollte im ersten Migrationsschritt nicht zwangsweise neu gebaut werden, ist aber der groesste technische Risikopunkt.

Minimalmigration:

- GPIO- und Watchdog-Zugriffe auf oeffentliche APIs umstellen.
- Bestehendes Bit-Banging erhalten.
- Timing auf echter Hardware messen.

Robuster Umbau, falls noetig:

- kritische Abschnitte verkleinern
- dauerhaft deaktivierte Interrupts vermeiden
- Bus-Erfassung ueber Interrupt-/Peripheral-Strategie neu bewerten
- Task-WDT und Idle-Task-WDT bewusst konfigurieren

## Betroffene Dateien

- `CMakeLists.txt`
- `main/CMakeLists.txt`
- `main/IntexSWG.cpp`
- `main/IntexSWG.h`
- `main/utils.h`
- `main/utils.cpp`
- `main/TM1650.cpp`
- `main/RestServer.cpp`
- `components/esp32-wifi-manager/CMakeLists.txt`
- `components/esp32-wifi-manager/component.mk`
- `components/esp32-wifi-manager/src/component.mk`
- `components/esp32-wifi-manager/src/wifi_manager.c`
- `sdkconfig`
- `partitions.csv`

## Verifikation

Nach der Migration sollten diese Checks laufen:

1. Referenzbuild mit ESP-IDF 4.4.x, falls moeglich.
2. Neuer Build mit aktueller ESP-IDF:

   ```sh
   idf.py set-target esp32
   idf.py reconfigure
   idf.py build
   ```

3. Statische Suche nach alten APIs:

   ```sh
   rg "esp_spi_flash|spi_flash_get|TIMERG|REG_WRITE|GPIO_OUT_W1|soc/timer_group|tcpip_adapter|SYSTEM_EVENT" main components
   ```

4. Build-Warnungen gezielt pruefen:

   ```sh
   idf.py build 2>&1 | rg -i "deprecated|obsolete|implicit|warning|error"
   ```

5. Flash- und Monitor-Test:

   ```sh
   idf.py flash monitor
   ```

6. REST-Funktionstest mit den vorhandenen Endpunkten aus `resturl.http`.
7. OTA-Test mit gueltiger und ungueltiger Firmware.
8. Hardware-Test fuer SWG-Bus, TM1650-Display, Relay und virtuelle Tastendruecke.
9. Stabilitaetstest ueber mindestens 30 Minuten mit WiFi, REST-Zugriffen und aktivem Bus.
10. Timingvergleich auf `dataPin` und `clockPin` vor und nach dem GPIO-Refactoring.

## Entscheidungen

- Ziel ist eine aktuelle stabile ESP-IDF-Version fuer ESP32.
- Keine Rueckwaertskompatibilitaet zu ESP-IDF 3.x einplanen.
- ESP-IDF-4.4.x-Kompatibilitaet nur erhalten, wenn sie explizit weiter gebraucht wird.
- REST-API, Web-Assets, Partition-Layout und bestehendes Geraeteverhalten bleiben erhalten.
- Kein grosser Protokoll-Neubau im ersten Schritt; Core1-Bus-Architektur nur dann groesser umbauen, wenn Timing oder Watchdog-Verhalten nach der Minimalmigration instabil bleiben.

## Groesste Risiken

1. Timing des bit-gebangten SWG-/TM1650-Busses nach Entfernung der Registermakros.
2. Watchdog-Verhalten der Core1-Schleife unter aktueller ESP-IDF.
3. mDNS-Abhaengigkeit, falls sie nicht mehr direkt aus der ESP-IDF-Installation verfuegbar ist.
4. OTA-Fehlerpfade, weil aktuelle IDF-Versionen Fehler strikter sichtbar machen koennen.
5. Stack-Groessen fuer HTTPD, OTA, cJSON und C++-Code.