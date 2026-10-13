## k85OS 7.8 (security update)

A hardening release. No new features; every user of the Web Terminal, the hotspot web file manager or FTP should update.

### Security
- **Web Terminal and hotspot web file manager:** the login cookie now carries a random per-login session token instead of the access key itself, with `HttpOnly` and `SameSite=Strict` (this also blocks cross-site requests to `/api/exec`, `/delete`, `/mkdir`, `/firmware/pull`). The hotspot no longer accepts the access key in the URL, and a valid session is no longer locked out by other people's failed attempts. A new key ends all old sessions.
- **Web Terminal:** it can no longer be started on an open (password-less) access point.
- **FTP:** the password is stored as a salted PBKDF2-SHA256 hash (old logins are upgraded automatically on the first successful login); new passwords must have at least 8 characters; after 6 failed logins in total, logins are locked for 5 minutes.
- **Hotspot web file manager:** uploads, deletes and new folders can no longer touch protected files (configuration, services, keys). Uploads go through a temporary file, so a failed or interrupted upload no longer deletes an existing file, and a full filesystem is reported as a failure.
- **Firmware flashing:** a slot that failed signature verification (or an aborted write) is invalidated immediately, so it can never be activated later through "Alt Firmware".
- **Lock screen password:** constant-time hash comparison; a failed key derivation can never match.
- The stale SSH host key left over from earlier versions is deleted from internal storage at boot.

### Fixed
- **Update check:** versions are now compared segment by segment (7.8.1 is newer than 7.8, 7.10 is newer than 7.9). Before, only "major.minor" was read, so a three-part version would never be offered as an update.
- The GitHub API reply buffers are now 32 KB (releases with several assets could overflow the old 4 KB buffer), and an overflowing reply fails cleanly instead of producing broken JSON.
- About 24 KB of internal RAM freed by moving large static buffers to PSRAM.

### Notes
- Log in to the hotspot web file manager again after updating (sessions are new).
- Existing short FTP passwords keep working; set a new one with `ftppass <user> <password>` if you want 8+ characters. Set it from the on-device Terminal rather than the Web Terminal, because the Web Terminal sends commands in the URL over plain HTTP.
