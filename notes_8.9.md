## k85OS 8.9

### Added
- **Web mode without a display.** If the screen is broken or missing, the stick draws into RAM and shows the screen in a browser: join the open WiFi `k85OS-XXXX`, open `http://192.168.4.1` (or `http://k85os.local`). The login is confirmed by pressing **A+B on the stick**; one browser session at a time. The page shows the screen full-window, supports taps and drag / mouse-wheel scrolling in every screen, and has a gear menu: upload a file to LittleFS, play music from the stick on the phone, open the BIOS, "back" (A+B), reboot, colours, language, RAM readout. Off by default: turn it on with `headless on` (shell), from the page, or hold **A+B at power-on**.
- **KStore** (main menu): browse `kion85/apps_k85os` and install from it. A `.bin` goes into the free OTA slot (signed with its `.sig`, or unsigned after an explicit confirmation, never activated), `.json` games run right away, `.thm` themes, `.lua` scripts.
- **Lua 5.4** (MIT): menu **Lua**, shell command `lua file.lua` / `lua -e "code"`, usable from `startup.sh` and cron. Drawing, buttons, files, shell and sound functions included; scripts live in `/littlefs/scripts`.
- **Messenger** (main menu): offline chat and file transfer between two sticks over ESP-NOW, no router needed. Everyone / private chats, file transfer with per-chunk acknowledgements and CRC32, selectable channel, optional file receiving (`/littlefs/inbox`).
- **WiFi analyzer**: channel load bars, AP list and a radar sweep; A switches views, B opens the radar, hold A+B to exit.
- **Russian interface**: Russian font, working RU on-screen keyboard (CAPS, whole-letter delete), language switch (`lang ru|en`, or on the web page) and a phrase translator hooked into the text output, so Settings, BIOS and the menus are translated.
- **Notes** (main menu): a simple notes app. New / view / edit / delete, multi-line text with the on-screen keyboard (RU/EN), up to 24 notes of about 760 bytes each, stored in LittleFS (`notes.dat`), newest first. Fully translated to Russian.
- Shell commands: `lua`, `mem` (internal RAM / PSRAM and the change since the last call), `headless`, `lang`.

### Changed
- About 70 KB of internal RAM freed by moving large static menu buffers to PSRAM; mbedTLS and the Bluetooth stack now allocate from PSRAM. BLE tools keep the WiFi AP running when enough internal RAM is free.
- Tools -> Performance now shows the CPU load (FreeRTOS run-time stats enabled).
- KStore and other HTTPS screens show the real reason when a request fails.

### Fixed
- Task watchdog trips in the music player and other long screens when the display is not attached.
- Crash on the lock screen caused by running out of internal RAM at boot.

### Notes
- Lua 5.4 is third-party code under the MIT license (copyright PUC-Rio), license text in the source headers.
- The web mode serves an open WiFi network; the physical A+B confirmation is the only protection. Do not leave it on in public places.