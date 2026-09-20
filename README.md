# k85OS

Custom firmware for M5Stack-family devices (ESP32-S3), written in C/C++ on ESP-IDF v5.3.1.

Originally ported from an earlier MicroPython/UIFlow prototype into a modular ESP-IDF C++ codebase.

## Supported Targets

ESP32-S3 (tested on ESP32-S3-PICO-1-N8R8, 8MB Flash + 8MB Octal PSRAM)

## Features

### Store (games)

- Puzzle: 2048, Tetris
- Arcade: Snake, Memory Match
- Classic: Reaction test, Flappy, Balance
- Pong

### Standalone apps

- Cube (3D wireframe demo)
- Colors, Clock (NTP-aware with uptime fallback)

### Tools

- WiFi Connect / WiFi Manager (up to 3 saved networks)
- Terminal (system commands: Free RAM, Uptime, WiFi status, Reboot)
- Bluetooth Scan, I2C Scanner, GPIO Control
- Files browser, Music Player, Melodies, Mic Test
- Air Mouse (IMU-based cursor), WiFi Hotspot, Calculator

### Settings

- Theme, battery mode, brightness, sound volume
- Device name, screen rotation, boot style
- High scores, factory reset, sleep modes, flashlight
- Cursor mode (IMU-driven pointer) and Desktop/OS mode (icon desktop + taskbar)

## Theming (.thm files)

UEFI/BIOS themes are plain-text `.thm` files: a single line, fields separated by `|`.

```
Name|Background|Text|Accent|Dark|Icon
```

| # | Field | Format | Example |
|---|-------|--------|---------|
| 1 | Theme name | text, up to 23 chars | `MyUEFI` |
| 2 | Background color (gradient top) | HEX without `#` | `000040` |
| 3 | Normal text color | HEX without `#` | `FFFFFF` |
| 4 | Selected item highlight color | HEX without `#` | `4444FF` |
| 5 | Dark theme | `1` or `0` | `1` |
| 6 | Battery icon | `bar` or `bolt` | `bolt` |

## Project Structure

```
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main
    ├── CMakeLists.txt
    ├── k85os_idf.cpp   # entry point
    ├── core/           # config, theme, input, power, sound, boot screen, rtc/ntp, cursor
    ├── ui/             # menu, list_menu, settings_menu, text_input
    ├── net/            # wifi
    ├── apps/
    │   ├── games/      # 2048, tetris, snake, reaction, flappy, race
    │   └── tools/      # terminal, wifi_manager, gpio_control, i2c_scanner, etc.
    └── system/, steps/ # system info, step counter
```

## Build

Requires ESP-IDF v5.3.1.

```
idf.py set-target esp32s3
idf.py build
idf.py -p COM_PORT flash
idf.py -p COM_PORT monitor
```

## Notes

- PSRAM must be configured as Octal mode in `menuconfig` (`Component config → ESP PSRAM`) for N8R8-variant chips - Quad mode will not work correctly on this hardware.
- Config/high scores are stored on-device via LittleFS (`storage` partition, see `partitions.csv`).

## License

MIT - see [LICENSE](https://github.com/kion85/k85OS/blob/master/LICENSE)

## FAQ

### Why doesn't this firmware have features like Bruce?

I don't have any real desire for my firmware to be able to attack anyone, capture handshakes, and so on. What I actually want is for these sticks to have something like a PC-style OS - not a full-blown OS, just a lightweight take on the idea. The firmware is built so you can use the device the way you'd use a PC, just packed into a tiny microcontroller.

Good luck to everyone, and I wish you all the very best in life!

P.S.: SSH support will work similarly to Bruce's implementation. Apologies if that earlier led anyone to the wrong idea.
