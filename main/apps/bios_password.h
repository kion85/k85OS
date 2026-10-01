#pragma once

// Вызывается при входе в BIOS. true = можно входить (пароль не задан или введён верно).
bool k85_bios_password_gate(void);

// Меню управления паролем BIOS (задать / сменить / удалить).
void k85_run_bios_password_menu(void);
