## k85OS 9.0

### Added
- **Drone** (Games): fly a drone, collect coins, dodge planes, birds, balloons and pylons. Hold A = up, hold B = down, double A = menu, double B = boost (3 s for 8 coins). Gear menu in the game: settings, save / load, new game, best / last, motor sound. In the web mode the page shows a stick (or arrows) and a BOOST button.
- **Realistic games** (all except Dash and Reaction): Flappy, Pong and Race with real physics and sounds, Snake with smooth movement and a timed golden apple, Tetris with 7 pieces, ghost piece, levels and a next-piece preview, 2048 with sliding and merging animation, Memory with card flips and shapes. Tetris controls changed: click A = left, click B = right, hold A = rotate, hold B = hard drop.
- **On-screen keyboard 2**: Shift, CAPS, a Fn layer with all the symbols, a real space bar, Enter, a text cursor (edit in the middle of the text), selection, copy / cut / paste and a clipboard with the 5 latest copies (Edt layer). Taps from the web page press the keys directly.
- **JSON viewer**: opens any JSON (comments, trailing commas, single quotes, bare keys, NaN, undefined, BOM, JSON Lines) as a tree; search, copy value / path, save a formatted copy, run as a game. The game interpreter accepts such JSON too and shows a file that is not a game as data.
- **Local HTML**: the Browser opens `file:///littlefs/...` and `/sd/...` pages (Go to URL accepts a plain path); Files -> Open/Run opens json, html, txt, md, csv, log and other text files.
- **System language** in Settings (English / Russian).
- **Hold A = step back** in Settings, BIOS, clock, quick settings, status bar settings, logs, files, music player and the Messenger list (lists and the main menu already had it).
- **Web mode**: the gear panel scrolls; in games the page shows A / B buttons (B left, A right) or touch zones; an optional AP password (WPA2/WPA3 mixed mode): `headless pass <password>` or the button on the page; **hold A+B at power-on for 3 s to switch the web mode on or off** (two rising beeps = on, two falling beeps = off).
- **Encrypted settings backup** (AES-256-GCM, PBKDF2): Settings -> Backup asks for a password; old plain backups still restore.
- **Signed KStore scripts**: a downloaded `.lua` runs without a warning only with a valid `<name>.lua.sig`; an unsigned script is shown as UNSIGNED and is not run by default.

### Changed
- The protected file list now also covers the copies of the config (`.bak`, `.new`, `.prev`) and the SD backup, ignores letter case and is shared by the shell, FTP, the web file manager, Lua, the messenger and KStore; `ls` hides protected files.
- The lock screen waits longer after every wrong password (2 s, 4 s ... 30 s).
- The web radio and the web terminal access points use the WPA2/WPA3 mixed mode; session tokens are compared in constant time.
- Console logs are compact (warnings and errors only): about 90 KB of the OTA slot freed.
- The release contains a full flash image `k85OS_9.0_full.bin` (bootloader + partition table + app, address 0x0) for M5Burner and esptool, and `bootloader_9.0.zip` (the second stage bootloader).

### Fixed
- A memory leak of the frame buffers when the web mode started with little free memory.
- A crash risk in the FTP server while it waits for an IP address.
- The file editor said "Saved!" on a full disk; Wi-Fi profile SSID and password copies could lose their terminator; Notes could be lost after a power cut while saving.
- A negative `Range` in the audio stream, a header over-read in the web server, and a status page that stayed broken after one failed allocation.

### Notes
- Writing the full image resets NVS: saved Wi-Fi passwords must be entered again; files in LittleFS (notes, scripts, config) are kept. `k85OS_9.0.bin` is the OTA update (the application only): do not flash it at 0x0.
- The bootloader is not updated by OTA. `bootloader_9.0.zip` is for updating it over USB (see README.txt inside); the full image already contains it.
- Hold A+B at power-on for 3 s to switch the web mode (access point `k85OS-XXXX`, http://192.168.4.1) on or off, for example when the display is broken.