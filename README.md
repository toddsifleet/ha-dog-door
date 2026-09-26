# MQTT Dog Door Controller

Firmware for an Arduino UNO R4 WiFi that raises and lowers a dog door and exposes it to Home Assistant as a cover, the same entity type used for garage doors and window shades. Home Assistant discovers the door over MQTT with no YAML, and a physical button on the door works whether or not the network is up.

## Demo

https://github.com/user-attachments/assets/04db9c3c-1672-4850-8f80-c61b44e2538a

## How it works

The motor drives a small winch. Opening winds a string that lifts the door until it trips a limit switch at the top. Closing unwinds the string for a fixed time and gravity lowers the door. There is no closed-position switch; "closed" means the door is not on the open switch. The gearbox cannot be back-driven, so the door holds wherever the motor stops.

The controller:

- Publishes MQTT discovery so the door appears in Home Assistant automatically, with open, close and stop.
- Reports opening, closing, open, closed and stopped states, retained on the broker so Home Assistant has the current state after its own restart.
- Publishes availability with a last-will message, so a board that drops off the network shows as unavailable rather than frozen on a stale state.
- Polls a debounced pushbutton that toggles the door: stop if moving, otherwise close if open, open if closed.
- Stops the motor from a hardware interrupt the moment the limit switch trips while opening. If a close overruns and rewinds the string the other way, the same switch stops it after a short arming delay.
- Keeps the button responsive when the network is down. WiFi and broker connection attempts are short, rate limited, backed off, and never made while the motor is running.

## Hardware

| Part                                                                                                                 | Notes                    |
| -------------------------------------------------------------------------------------------------------------------- | ------------------------ |
| [Arduino UNO R4 WiFi](https://store-usa.arduino.cc/products/uno-r4-wifi)                                             | 2.4 GHz only             |
| [Arduino Motor Shield Rev3](https://store-usa.arduino.cc/products/arduino-motor-shield-rev3)                         | Channel A                |
| [Mechanical limit switch](https://www.amazon.com/HUAREW-Vertical-Mechanical-3018-PROVer-3018-MX3/dp/B0B38YP135?th=1) | Open position            |
| [DC gear motor](https://www.amazon.com/dp/B07F8TGB9L?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_11&th=1)             | Drives the winch         |
| [Dog door](https://www.amazon.com/dp/B0CNCRBRM1?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_4&th=1)                   |                          |
| Momentary pushbutton                                                                                                 | Between pin 7 and ground |

### Pin map

| Pin | Function          | Notes                        |
| --- | ----------------- | ---------------------------- |
| D3  | Motor PWM         | Motor shield channel A       |
| D9  | Motor brake       | Motor shield channel A       |
| D12 | Motor direction   | LOW opens, HIGH closes       |
| D2  | Open limit switch | Rising edge interrupt        |
| D7  | Pushbutton        | Internal pull-up, active low |

## Configuration

Credentials live in `src/secrets.h`, which is ignored by git. Create it from the template:

```bash
cp src/secrets.h.example src/secrets.h
```

| Setting                          | Meaning                                                                                                                 |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `UNIQUE_ID`                      | Identifier for the cover entity. Changing it creates a new entity in Home Assistant.                                    |
| `DEVICE_NAME`                    | Name shown in Home Assistant for both the device and the cover.                                                         |
| `WIFI_SSID`, `WIFI_PASSWORD`     | Must be a 2.4 GHz network. On mesh systems that merge bands, use a 2.4 GHz-only or IoT SSID if the board fails to join. |
| `MQTT_IP`                        | Broker address. Give the broker a DHCP reservation so this never changes.                                               |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | A Home Assistant user, or a user defined in the Mosquitto add-on.                                                       |

Timing constants, such as the close duration, the limit switch arming delay and the network retry intervals, are at the top of `src/main.cpp` with comments explaining each.

## Building and flashing with PlatformIO

The project is a standard [PlatformIO](https://platformio.org) project. The board, framework and library dependencies are declared in `platformio.ini`, and PlatformIO fetches the toolchain and libraries on the first build.

### Install PlatformIO

Either:

- Install the **PlatformIO IDE** extension in VS Code. The repository recommends it in `.vscode/extensions.json`, so VS Code will offer it when the folder is opened. The extension adds Build, Upload and Monitor buttons to the status bar and installs the CLI alongside.
- Or install the **PlatformIO Core** CLI on its own, following the [installation guide](https://docs.platformio.org/en/latest/core/installation/index.html).

The CLI is installed at `~/.platformio/penv/bin/pio`. Add that directory to your `PATH`, or use the full path in the commands below.

### Commands

Run these from the repository root.

Build the firmware:

```bash
pio run
```

Build and upload to a board connected over USB:

```bash
pio run -t upload
```

Open the serial monitor. Exit with Ctrl+C.

```bash
pio device monitor -b 115200
```

Build, upload and open the monitor in one step:

```bash
pio run -t upload && pio device monitor -b 115200
```

Clean build output:

```bash
pio run -t clean
```

Show the connected boards and their serial ports:

```bash
pio device list
```

## Future work

- A way for the dog to tell that the door is closed from the outside.

## License

MIT. See [LICENSE](LICENSE).
