## k85OS 9.3

Security and reliability release (full code audit since 9.0). No new apps, many fixes.

### Security
- Protected files (config, backup, services file) are now also protected in the on-device file manager (they are hidden and cannot be read, copied, renamed or deleted) and in the web file manager (rename was a bypass)
- FAT 8.3 short names (for example K85OS_~1.JSO) are treated as protected paths
- Web file manager: names with `<` `>` `"` `'` `\` are refused or hidden; a client that stops reading can no longer hang the server on a download; the login form is read completely even when it arrives in a separate packet
- Lua scripts: `load()` accepts text chunks only (no binary bytecode), `string.dump` removed
- Backup password must have at least 8 characters (was 4)
- A damaged or replaced config can no longer cause an overflow (rotation, active profile and password-manager entry length are checked on load)
- Downloads (store, wget, repo) can no longer fill LittleFS: 96 KB always stays free for settings

### Fixes
- The config is saved atomically (temp file + rename): a power loss during a save no longer wipes the settings and the lock password; the file size is checked on load
- Services file: limit raised to 2 KB, and a file that does not fit is never written (a lost AP password would have opened the AP)
- Web Radio: leaving during a scan no longer risks a crash; a failed socket is handled
- Hotspot and Router: a busy port 80 is reported instead of failing silently
- App repo: a response that is too big no longer produces a plausible but broken JSON
- File editor: files bigger than 2 KB are no longer cut when saved (the editor refuses them)
- TOTP: fixed a signed overflow when decoding long Base32 secrets

### Notes
- Update over the air as usual (signed .bin + .sig). Full image and bootloader are attached for a USB install.
- Open items for the next releases: password for the Web Radio page, key in the router admin URL, audio buffering in Web Radio, ESP-NOW file receive is still on by default.