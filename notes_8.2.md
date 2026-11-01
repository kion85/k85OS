## k85OS 8.2

Customization release: wallpapers, a rebuilt lock screen, script games and a round of fixes.

### Added
- **Look** (Settings): your own wallpapers for the lock screen, the main menu and every other screen, and a photo banner behind the status bar. Put images (JPG, PNG, BMP or QOI) into `/littlefs/wall` with FTP or the web file manager and choose them in Settings -> Look; each image has a dimming level so the text stays readable. An image is decoded once and kept as a ready frame, so screens open without delay.
- **Lock screen rebuilt**: smooth particles without flicker (every frame is composed off-screen), 8 built-in scenes (Classic, Snow, Stars, Sparks, Letters, Lights, Hearts, Rain), your own scenes as JSON files in `/littlefs/lock` (an example and a description are created there), shape, speed, density and color settings, bursts on a button press or a shake, a preview, and three battery modes (Saver, Balanced, Smooth) that lower the frame rate and dim the backlight while the stick is idle.
- **Script games** in Tools -> Interpreter: graphics and logic from a JSON file (sprites, objects with collisions, expressions, scenes, sounds and melodies, the best score is saved), files up to 256 KB and clear error messages with the line and column. `dodger.json` and a description of the format are created in `/littlefs/games_json`. Text quests and grid games work as before.
- **Status bar** (Settings -> Status bar) is now drawn on every list screen, on messages and in the apps, with the same settings.
- **Back to k85OS without a cable**: hold A while switching the stick on and the bootloader selects the k85OS slot, even if a third-party firmware keeps booting. This needs a one-time USB flash (`idf.py flash`), because over-the-air updates do not change the bootloader.
- **I2C scanner** scans the Grove port and the internal bus through the buses of M5Unified (the old pins were wrong for the StickS3) and knows more sensors.

### Fixed
- **Voice pack**: the download stopped at about 3.4 MB because the pack had grown past its size limit, and it was retried 60 times; the limit is 6 MB now, the reason is logged and retries stop when the pack is too big.
- **Router**: the admin page and the DNS filter answer only on the hotspot address (not on the home network side), DNS answers larger than 512 bytes are no longer cut, the access key is compared in constant time and DNS or access-list entries accept only plain characters.
- **Shell `tar`**: archive entries over 64 MB are rejected (a damaged archive could loop forever).
- **Log and updates**: log lines written from several tasks no longer overwrite each other, and the background update check never runs more often than once a minute.
- **Password manager and the saved WiFi password**: a failed key derivation is detected, the master password is checked in constant time, keys are wiped from memory after use, damaged data gives an empty result instead of garbage, and the WiFi store key reads the MAC from eFuse.

### Notes
- An over-the-air update installs the application only; the partition table and the bootloader are not changed.
- Your look is kept in small files in LittleFS (`k85_look.txt`, `k85_lockfx.txt`, the `wall` and `lock` folders): copy them to keep it.

- Firmware is signed (Ed25519): `k85OS_8.2.bin` + `k85OS_8.2.bin.sig`.
