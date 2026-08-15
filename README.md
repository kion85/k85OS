firmware for M5stickS3
# k85OS

Custom firmware for M5Stack-family devices (ESP32-S3), written in C++ on ESP-IDF v5.3.1.

Originally ported from an earlier MicroPython/UIFlow prototype into a modular ESP-IDF C++ codebase.

## Supported Targets

ESP32-S3 (tested on ESP32-S3-PICO-1-N8R8, 8MB Flash + 8MB Octal PSRAM)

№№ Theme
Инструкция: как писать .thm-файлы для UEFI/BIOS

Формат - одна строка текста, поля через | (вертикальная черта):

Имя|Фон|Текст|Акцент|Тёмная|Иконка
#	Что	Формат	Пример
1	Имя темы	текст, до 23 симв.	MyUEFI
2	Цвет фона (верх градиента)	HEX без #	000040
3	Цвет обычного текста	HEX без #	FFFFFF
4	Цвет подсветки выбранного пункта	HEX без #	4444FF
5	Тёмная тема	1 или 0	1
6	Иконка батареи	bar или bolt	bolt

## Features

**Store (games)**

- Puzzle: 2048, Tetris

- Arcade: Snake, Memory Match

- Classic: Reaction test, Flappy, Balance

- Pong

**Standalone apps**

- Cube (3D wireframe demo)

- Colors, Clock (NTP-aware with uptime fallback)

**Tools**

- WiFi Connect / WiFi Manager (up to 3 saved networks)

- Terminal (system commands: Free RAM, Uptime, WiFi status, Reboot)

- Bluetooth Scan, I2C Scanner, GPIO Control

- Files browser, Music Player, Melodies, Mic Test

- Air Mouse (IMU-based cursor), WiFi Hotspot, Calculator

**Settings**

- Theme, battery mode, brightness, sound volume

- Device name, screen rotation, boot style

- High scores, factory reset, sleep modes, flashlight

## Project Structure

├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main
├── CMakeLists.txt
├── k85os_idf.cpp # entry point
├── core/ # config, theme, input, power, sound, boot screen, rtc/ntp
├── ui/ # menu, list_menu, settings_menu, text_input
├── net/ # wifi
├── apps/
│ ├── games/ # 2048, tetris, snake, reaction, flappy, race
│ └── tools/ # terminal, wifi_manager, gpio_control, i2c_scanner, etc.
└── system/, steps/ # system info, step counter

## Build

Requires ESP-IDF v5.3.1.

```powershell

idf.py set-target esp32s3

idf.py build

idf.py -p COM_PORT flash

idf.py -p COM_PORT monitor

```

## Notes

- PSRAM must be configured as **Octal mode** in `menuconfig` (`Component config → ESP PSRAM`) for N8R8-variant chips - Quad mode will not work correctly on this hardware.

- Config/high scores are stored on-device via LittleFS (`storage` partition, see `partitions.csv`).

## License

MIT - see [LICENSE](LICENSE)

## отдельно..

### ПОЧЕМУ ПРОШИВКА НЕ ИМЕЕТ ФУНКЦИИ КАК В Bruce? ###
 ## дело в том что я не имею огромное желание чтобы моя прошивка могла атаковать кого забирать хэншдейки и так далее, не я просто хочу что бы для стиков было ос похожая на ПК но не прям полноценная ОС а просто так его реализация, и прошивка заточена что бы вы могли пользоваться как ос, то есть ПК но внутри маленького микроконтроллера,а так всем удачи и желаю всего вам наилучшего в жизни! ##
