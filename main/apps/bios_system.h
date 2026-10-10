#pragma once
// BIOS -> System tools -> Restore points: named copies of the config with date.
void k85_run_restore_points(void);
// BIOS -> System tools -> Integrity check: SHA-256 verification of every firmware slot.
void k85_run_integrity_check(void);
