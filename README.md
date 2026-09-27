# ToGrill display for the ESP32-2432S032

ESP-IDF 6.1 firmware for the same Cheap Yellow Display board as the P2000 CYD port: ESP32-WROOM, ST7789, 320×240 landscape. It connects to a ToGrill base (service `CEE0`), shows four probes and ambient temperature, and publishes them over MQTT. Tap **Setup** to set the Wi-Fi name, Wi-Fi password, MQTT broker, and MQTT login. Those values are stored on the device and replace the menuconfig defaults after the first save.

The phone app cannot read the thermometer while this link is held. The touch controller is the GT911 used on the capacitive P2000 board (SDA 33, SCL 32).

## Configure

This tree builds with the ESP-IDF 6.1 install already on this machine:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.1.sh"
idf.py set-target esp32
idf.py menuconfig
```

Under **ToGrill CYD** set the Wi-Fi SSID, the MQTT URI (default `mqtt://192.168.1.1:1883`), and optionally the thermometer MAC. Leave the MAC empty to take the first advertisement that looks like a ToGrill device. `sdkconfig` stays local and is gitignored, so the Wi-Fi password is not committed.

## Flash

```sh
idf.py build flash monitor
```

## MQTT

Retained topics, with prefix `togrill` unless you change it:

| Topic | Payload |
| --- | --- |
| `togrill/status` | `online` or `offline` (last will) |
| `togrill/state` | JSON with `probes`, `ambient`, `battery`, `ble` |
| `togrill/probe/1` | Celsius with one decimal, or `none` |
| `togrill/ambient` | Celsius, when the base reports an ambient sensor |
| `togrill/battery` | Percent |

The protocol matches the public `togrill-bluetooth` library: framed writes to `CEE1`, notifications on `CEE2`. A fully wireless probe that does not speak this protocol will not show temperatures.
