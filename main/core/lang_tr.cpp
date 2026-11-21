// K85RU2: phrase translator hooked into the M5GFX text output (works for every screen that prints text)
#include "lang.h"
#include "M5Unified.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

struct Ph { const char *en; const char *ru; };
static const Ph k_ph[] = {
    /* K85SYSLANG1 */ {"System language","Язык системы"}, {"Russian","Русский"}, {"English","Английский"},
    /* K85CLIP1 */ {"Clipboard","Буфер обмена"}, {"Clear clipboard","Очистить буфер"}, {"Copied","Скопировано"}, {"Pasted","Вставлено"}, {"Cut to clip","Вырезано"}, {"Clip empty","Буфер пуст"}, {"Empty text","Текст пуст"}, {"No room","Нет места"},
    /* K85NOTES2: Notes app */ {"Notes","Заметки"}, {"+ New note","+ Новая заметка"}, {"New note","Новая заметка"}, {"Edit note","Правка заметки"}, {"View note","Просмотр заметки"}, {"Delete note","Удалить заметку"}, {"Back to list","Назад к списку"}, {"Keep it","Оставить"}, {"Yes, delete","Да, удалить"}, {"Notes full","Список полон"}, {"No memory","Нет памяти"}, {"Empty: not saved","Пусто: не сохранено"}, {"Save failed","Ошибка сохранения"}, {"(empty)","(пусто)"},
    /* hints */
    {"A+B (hold) = exit","A+B (удерж.) = выход"}, {"A+B hold to unlock","A+B удерж. = разблок."}, {"A+B=back","A+B=назад"},
    {"A+B=cancel","A+B=отмена"}, {"A+B=exit","A+B=выход"}, {"A=+10%","A=+10%"}, {"A=cycle B=apply&back","A=смена B=ОК и назад"},
    {"A=next B=boot","A=далее B=загрузка"}, {"A=next B=change/back A+B=exit","A=далее B=смена/назад A+B=выход"},
    {"A=next B=select A+B=exit","A=далее B=выбор A+B=выход"}, {"A=next B=select","A=далее B=выбор"},
    {"A=next B=toggle A+B=back","A=далее B=перекл. A+B=назад"}, {"A=scroll A+B=exit","A=прокрутка A+B=выход"},
    {"A=select B=confirm","A=выбор B=подтв."}, {"B=next  A+B=exit","B=далее  A+B=выход"}, {"B=page A+B=back","B=стр. A+B=назад"},
    {"B=restore  A=cancel","B=восстан. A=отмена"}, {"B=save  A=cancel","B=сохр. A=отмена"},
    {"B=SD speed test   A+B=exit","B=тест SD   A+B=выход"}, {"B=start  A=cancel","B=старт  A=отмена"},
    {"B=confirm A+B=cancel","B=да A+B=отмена"}, {"B=confirm","B=да"}, {"B=install","B=установить"}, {"Hold A+B to exit","Удерж. A+B = выход"},
    {"Hold BOTH 1s = exit","Удерж. обе 1с = выход"}, {"Press A+B to continue booting","A+B = продолжить загрузку"},
    {"Use A to move, B to boot","A = выбор, B = загрузка"}, {"A=next","A=далее"}, {"B=select","B=выбор"},
    /* common words and short phrases */
    {"Voltage:","Напряжение:"}, {"(chg)","(заряд)"}, {"(off)","(выкл)"}, {"(on)","(вкл)"}, {"(running)","(работает)"},
    {"(no date)","(нет даты)"}, {"empty or invalid image","пустой или неверный образ"}, {"ver:","верс.:"}, {"app:","прил.:"},
    {"built:","собр.:"}, {"encrypted:","шифр.:"}, {"subtype","подтип"}, {"bytes written","байт записано"},
    {"(card full / locked?)","(карта полна/защищена?)"}, {"(port busy?)","(порт занят?)"}, {"*** KERNEL PANIC ***","*** ПАНИКА ЯДРА ***"},
    {"(oldest removed)","(старейшая удалена)"}, {"+ Create restore point","+ Создать точку"},
    {"2 asc tones: OK, no errors","2 растущих: ОК, без ошибок"}, {"2 high: RTC not found","2 высоких: нет RTC"},
    {"3 mid: battery low","3 средних: батарея разряжена"}, {"1 low+long: LittleFS fail","1 низкий длинный: ошибка LittleFS"},
    {"Accel","Ускор."}, {"Gyro","Гиро"}, {"Active OTA slot","Активный OTA-слот"}, {"All set!","Всё готово!"},
    {"Alternate Firmware","Альтернативная прошивка"}, {"Alt Firmware","Альт. прошивка"}, {"and try again","и повторите"},
    {"any key","любая клавиша"}, {"Applied:","Применено:"}, {"Auto download:","Автозагрузка:"}, {"Auto-boot in","Автозагрузка через"},
    {"Auto-boot","Автозагрузка"}, {"Automation / safe","Автоматика / защита"}, {"Autostart disabled","Автозапуск выключен"},
    {"Background color","Цвет фона"}, {"Backup config","Копия настроек"}, {"Backup failed","Ошибка копирования"},
    {"Bad file","Плохой файл"}, {"Banner:","Баннер:"}, {"BG gradient","Градиент фона"}, {"BG:","Фон:"},
    {"BIOS password set","Пароль BIOS задан"}, {"BIOS password","Пароль BIOS"}, {"BIOS PASSWORD","ПАРОЛЬ BIOS"}, {"removed","удалён"},
    {"Block download:","Блок. загрузок:"}, {"Bluetooth module","Модуль Bluetooth"}, {"Bluetooth Scan","Поиск Bluetooth"},
    {"Bluetooth status","Статус Bluetooth"}, {"Bluetooth: HID ready","Bluetooth: HID готов"}, {"Boot info","О загрузке"},
    {"BOOT INFO","О ЗАГРУЗКЕ"}, {"Boot loader style","Стиль загрузчика"}, {"Boot options","Параметры загрузки"},
    {"Boot other slot","Загрузить др. слот"}, {"BOOT SLOT NOW","ЗАГРУЗИТЬ СЛОТ"}, {"Boot style","Стиль загрузки"},
    {"booting in","загрузка через"}, {"Brightness test","Тест яркости"}, {"Brightness","Яркость"}, {"BROWNOUT (low power)","ПРОСАДКА ПИТАНИЯ"},
    {"Brownout (power supply fault)","Просадка питания"}, {"Burst:","Взрыв:"}, {"BUTTON TEST","ТЕСТ КНОПОК"}, {"BUTTON A","КНОПКА A"},
    {"BUTTON B","КНОПКА B"}, {"Button A/B test","Тест кнопок A/B"}, {"Button A:","Кнопка A:"}, {"Button B:","Кнопка B:"},
    {"Buttons test","Тест кнопок"}, {"presses:","нажатий:"}, {"Cannot decode","Не удалось декод."}, {"the image","изображение"},
    {"Cannot read the file","Не удалось прочитать файл"}, {"Change password","Сменить пароль"}, {"Check for updates","Проверить обновления"},
    {"Checking","Проверка"}, {"Chip info","О чипе"}, {"Chip temp","Темп. чипа"}, {"Chip:","Чип:"}, {"cores","ядра"},
    {"CHOOSE IMAGE","ВЫБОР ИЗОБРАЖЕНИЯ"}, {"Classic bar","Классическая полоса"}, {"Color Test","Тест цвета"}, {"Color:","Цвет:"},
    {"Config backed up","Настройки сохранены"}, {"(stays on device)","(остаётся на стике)"}, {"Config reset","Настройки сброшены"},
    {"Config restored","Настройки восстановлены"}, {"Rebooting...","Перезагрузка..."}, {"Connect failed","Не удалось подключиться"},
    {"(timeout)","(тайм-аут)"}, {"Connect the USB cable to a PC.","Подключите USB к ПК."}, {"Connect to WiFi first","Сначала подключите WiFi"},
    {"Connect WiFi first","Сначала подключите WiFi"}, {"Connected!","Подключено!"}, {"Connecting WiFi...","Подключение WiFi..."},
    {"Could not create","Не удалось создать"}, {"restore point","точку восстановления"}, {"Could not start USB","USB не запустился"},
    {"Could not write the file","Не удалось записать файл"}, {"CPU freq","Частота CPU"}, {"CPU frequency","Частота процессора"},
    {"CPU temp:","Темп. CPU:"}, {"Temp","Темп."}, {"Crash streak:","Серия сбоев:"}, {"Crash test","Тест сбоя"}, {"Crashing ...","Сбой..."},
    {"Cube (coords)","Куб (коорд.)"}, {"Current ones kept as .prev","Текущие сохранены как .prev"}, {"Current password","Текущий пароль"},
    {"DATA MISMATCH","ДАННЫЕ НЕ СОВПАДАЮТ"}, {"data OK","данные ОК"}, {"Date:","Дата:"}, {"Deep sleep wake","Выход из глубокого сна"},
    {"deep sleep wake","выход из сна"}, {"Delete this restore","Удалить эту точку"}, {"point?","восстановления?"}, {"Density:","Плотность:"},
    {"Device name","Имя устройства"}, {"Device recovered and rebooted.","Устройство восстановлено."}, {"Device:","Устройство:"},
    {"Did you hear","Вы слышали"}, {"3 rising tones?","3 возрастающих тона?"}, {"Dim:","Затемн.:"}, {"Disable all autostart","Выкл. весь автозапуск"},
    {"Disable screen lock?","Отключить блокировку?"}, {"Display test","Тест дисплея"}, {"Done","Готово"}, {"Download failed","Ошибка загрузки"},
    {"Download is already","Загрузка уже"}, {"Download now","Загрузить сейчас"}, {"Downloads are blocked","Загрузки заблокированы"},
    {"Eject the drive on the PC first!","Сначала извлеките диск на ПК!"}, {"Ejecting ...","Извлечение..."}, {"Enter password:","Введите пароль:"},
    {"Explosion:","Взрыв:"}, {"External pin","Внешний пин"}, {"external pin","внешний пин"}, {"FACTORY RESET (wipe all data)","ЗАВОДСКОЙ СБРОС (всё удалить)"},
    {"Factory reset done","Сброс выполнен"}, {"Factory reset","Сброс до заводских"}, {"Failed to set","Не удалось задать"}, {"boot slot","загрузочный слот"},
    {"Fetching theme list...","Загрузка списка тем..."}, {"File is too big","Файл слишком большой"}, {"Firmware signature:","Подпись прошивки:"},
    {"required","обязательна"}, {"Key SHA-256:","Ключ SHA-256:"}, {"FIRMWARE SLOTS","СЛОТЫ ПРОШИВКИ"}, {"flash drive on the computer.","флешка на компьютере."},
    {"Flash encryption:","Шифр. флеш:"}, {"Flash:","Флеш:"}, {"Flash1 (Normal)","Flash1 (Обычный)"}, {"Flash3 (Test)","Flash3 (Тест)"},
    {"Flash4 (Alt FW)","Flash4 (Альт. ПО)"}, {"Free RAM","Своб. ОЗУ"}, {"KB free","КБ своб."}, {"FTP autostart:","Автозапуск FTP:"},
    {"GPIO Control","Управление GPIO"}, {"GRUB menu","Меню GRUB"}, {"Hardware monitor","Монитор железа"}, {"HARDWARE MONITOR","МОНИТОР ЖЕЛЕЗА"},
    {"Hash failed","Ошибка хэша"}, {"Highlight:","Выделение:"}, {"I2C Scanner","Сканер I2C"}, {"Image is too large","Изображение слишком большое"},
    {"(max 16000 px)","(макс. 16000 px)"}, {"Image:","Изобр.:"}, {"IMU (tilt)","IMU (наклон)"}, {"IMU not found","IMU не найден"},
    {"IMU test","Тест IMU"}, {"IMU TEST","ТЕСТ IMU"}, {"Includes WiFi, TOTP, passwords!","Включая WiFi, TOTP, пароли!"},
    {"Insert a FAT32 card","Вставьте карту FAT32"}, {"Installing","Установка"}, {"INTEGRITY (* = running)","ЦЕЛОСТНОСТЬ (* = текущий)"},
    {"Integrity check","Проверка целостности"}, {"interrupt watchdog","прерывание watchdog"}, {"Interrupt watchdog timeout","Тайм-аут watchdog прерываний"},
    {"Interrupt WDT","WDT прерываний"}, {"k85OS (BIOS / recovery)","k85OS (BIOS / восстановление)"}, {"k85OS (normal)","k85OS (обычный)"},
    {"k85OS / FreeRTOS fault detected","Сбой k85OS / FreeRTOS"}, {"k85OS Boot Menu","Меню загрузки k85OS"}, {"k85OS Setup Utility","Утилита настройки k85OS"},
    {"not found","не найден"}, {"Kernel panic (unhandled exception)","Паника ядра (необработанное исключение)"}, {"Keyboard nav","Навигация клавиат."},
    {"Largest RAM block","Макс. блок ОЗУ"}, {"Last reset:","Посл. сброс:"}, {"Let's set things up","Давайте настроим"}, {"List+Icons","Список+значки"},
    {"used","занято"}, {"LOCK SCREEN","ЭКРАН БЛОКИРОВКИ"}, {"Lock screen","Экран блокировки"}, {"MAC address","MAC-адрес"}, {"Menu UI style","Стиль меню"},
    {"Mic Test","Тест микрофона"}, {"Min free heap:","Мин. свободно:"}, {"Mode:","Режим:"}, {"Model:","Модель:"}, {"Rev:","Ревизия:"},
    {"Cores:","Ядер:"}, {"Freq:","Частота:"}, {"Mute all sound","Выключить весь звук"}, {"My scene","Моя сцена"}, {"New password (4+)","Новый пароль (4+)"},
    {"No app partitions","Нет разделов приложений"}, {"No backup found","Копия не найдена"}, {"No backup","Нет копии"}, {"no hash","нет хэша"},
    {"No networks found","Сети не найдены"}, {"No notifications","Нет уведомлений"}, {"No other slot found","Другой слот не найден"},
    {"No other valid slot","Нет другого рабочего слота"}, {"No password set","Пароль не задан"}, {"No PSRAM found","PSRAM не найдена"},
    {"No saved WiFi","Нет сохранённых сетей"}, {"No SD card","Нет SD-карты"}, {"No speaker found","Динамик не найден"}, {"No themes found","Темы не найдены"},
    {"No update found","Обновлений нет"}, {"Not a JPG, PNG, BMP","Не JPG, PNG, BMP"}, {"or QOI image","или QOI"}, {"Not a k85OS backup","Это не копия k85OS"},
    {"Not enough free PSRAM","Мало свободной PSRAM"}, {"Not enough memory","Мало памяти"}, {"not set","не задан"}, {"Old settings kept","Старые настройки сохранены"},
    {"on the SD card","на SD-карте"}, {"OTA lock (soft)","Блок. OTA (прогр.)"}, {"other watchdog","другой watchdog"}, {"Other watchdog reset","Сброс другим watchdog"},
    {"Other WDT","Другой WDT"}, {"PANIC (crash)","ПАНИКА (сбой)"}, {"Partition table","Таблица разделов"}, {"Passwords differ","Пароли не совпадают"},
    {"PC connected","ПК подключён"}, {"pending verify","ожидает проверки"}, {"POST beep codes:","Коды POST-сигналов:"}, {"POST beep info","Инфо о POST-сигналах"},
    {"POST beep","POST-сигнал"}, {"Power info","Питание"}, {"POWER INFO","ПИТАНИЕ"}, {"Power off device?","Выключить устройство?"}, {"Power Off","Выключить"},
    {"Preparing...","Подготовка..."}, {"(can take a few","(может занять"}, {"seconds)","несколько секунд)"}, {"Profile:","Профиль:"},
    {"Progressive JPEG is","Прогрессивный JPEG"}, {"not supported.","не поддерживается."}, {"Save it as baseline","Сохраните как baseline"},
    {"PSRAM ERRORS:","ОШИБКИ PSRAM:"}, {"first at","первая по"}, {"tested","проверено"}, {"PSRAM OK","PSRAM в порядке"}, {"3 patterns, 0 errors","3 шаблона, 0 ошибок"},
    {"PSRAM test...","Тест PSRAM..."}, {"PSRAM test","Тест PSRAM"}, {"please wait","подождите"}, {"PSRAM: none","PSRAM: нет"},
    {"Radio check running...","Проверка радио..."}, {"RAM / ROM info","ОЗУ / ПЗУ"}, {"RAM:","ОЗУ:"}, {"read err","ошибка чтения"}, {"Read:","Чтение:"},
    {"Write FAILED (card locked/full?)","ОШИБКА записи (карта защищена/полна?)"}, {"Write:","Запись:"}, {"Reboot device?","Перезагрузить устройство?"},
    {"Reboot device","Перезагрузить"}, {"Remove password","Удалить пароль"}, {"Repeat password","Повторите пароль"}, {"Replaces ALL settings with the","Заменяет ВСЕ настройки"},
    {"Reset config to defaults","Сброс к настройкам по умолч."}, {"Reset config","Сбросить настройки"}, {"Reset steps","Сбросить шаги"}, {"Reset:","Сброс:"},
    {"Restarting ...","Перезапуск..."}, {"Restore config backup","Восстановить из копии"}, {"Restore config","Восстановить настройки"},
    {"Restore failed","Ошибка восстановления"}, {"Restore from SD","Восстановить с SD"}, {"RESTORE POINTS","ТОЧКИ ВОССТАНОВЛЕНИЯ"},
    {"Restore points","Точки восстановления"}, {"Restore point","Точка восст."}, {"created","создана"}, {"Device will reboot","Устройство перезагрузится"},
    {"Restoring ...","Восстановление..."}, {"Rollback failed","Ошибка отката"}, {"Rollback firmware","Откат прошивки"}, {"Rolling back...","Откат..."},
    {"ROM download:","Загрузка ROM:"}, {"Rotation test","Тест поворота"}, {"Rotation:","Поворот:"}, {"RTC not found","RTC не найден"}, {"RTC test","Тест RTC"},
    {"RTC TEST","ТЕСТ RTC"}, {"Running:","Работает:"}, {"Other slot:","Другой слот:"}, {"Save to SD","Сохранить на SD"},
    {"Saves all settings to the SD card","Сохраняет все настройки на SD-карту"}, {"Saving ...","Сохранение..."}, {"Scan & connect","Поиск сети"},
    {"Scene:","Сцена:"}, {"Scheduler:","Планировщик:"}, {"Screen lock disabled","Блокировка выключена"}, {"Screen lock enabled","Блокировка включена"},
    {"Screen lock","Блокировка экрана"}, {"mounting ...","монтирование..."}, {"no card / mount failed","нет карты / ошибка"}, {"Secure boot:","Защ. загрузка:"},
    {"Security status","Безопасность"}, {"SECURITY STATUS","БЕЗОПАСНОСТЬ"}, {"Select boot device:","Выберите загрузку:"}, {"Select:","Выбор:"},
    {"Set lock password:","Пароль блокировки:"}, {"Set password","Задать пароль"}, {"Set time zone","Часовой пояс"}, {"Settings loaded.","Настройки загружены."},
    {"SETUP: Device name","НАСТРОЙКА: Имя"}, {"SETUP: Theme","НАСТРОЙКА: Тема"}, {"SETUP: Time","НАСТРОЙКА: Время"}, {"SETUP: WiFi","НАСТРОЙКА: WiFi"},
    {"Shape:","Форма:"}, {"Signature info","Инфо о подписи"}, {"Skip all","Пропустить всё"}, {"Sleep now","Спать сейчас"}, {"Sleep wake:","Пробуждение:"},
    {"Slot details","О слоте"}, {"Slot:","Слот:"}, {"Slots & partitions","Слоты и разделы"}, {"SLOTS & PARTITIONS","СЛОТЫ И РАЗДЕЛЫ"},
    {"Software (reboot)","Программный (перезагр.)"}, {"software restart","программный перезапуск"}, {"Sound is muted or","Звук выключен или"},
    {"volume is 0","громкость 0"}, {"Sound volume","Громкость звука"}, {"Sound:","Звук:"}, {"Speaker test","Тест динамика"}, {"Speed:","Скорость:"},
    {"Spinner circle","Крутящийся круг"}, {"Starting ...","Запуск..."}, {"Startup script:","Скрипт запуска:"}, {"State:","Состояние:"}, {"Static text","Статичный текст"},
    {"Status bar items","Элементы строки"}, {"Status bar","Строка состояния"}, {"Steps:","Шаги:"}, {"Storage test","Тест хранилища"}, {"STORAGE TEST","ТЕСТ ХРАНИЛИЩА"},
    {"Storage:","Хранилище:"}, {"Syncing time...","Синхронизация времени..."}, {"SYSTEM CHECK","ПРОВЕРКА СИСТЕМЫ"},
    {"System check (batt/temp/radio)","Проверка системы (бат/темп/радио)"}, {"System log:","Журнал системы:"}, {"System tools","Системные инструменты"},
    {"SYSTEM TOOLS","СИСТЕМНЫЕ ИНСТРУМЕНТЫ"}, {"tap = burst, shake it","тап = взрыв, потрясите"}, {"Task watchdog timeout","Тайм-аут watchdog задач"},
    {"task watchdog","watchdog задач"}, {"Task WDT","WDT задач"}, {"TEST MODE","РЕЖИМ ТЕСТА"}, {"Test Mode","Режим теста"}, {"Text:","Текст:"},
    {"The SD card will appear as a","SD-карта появится как"}, {"Theme copy failed","Ошибка копирования темы"}, {"Theme:","Тема:"}, {"Time:","Время:"},
    {"Too many restore","Слишком много точек"}, {"Too short (min 4)","Слишком короткий (мин. 4)"}, {"UEFI THEMES","ТЕМЫ UEFI"}, {"UI Style:","Стиль UI:"},
    {"Unknown fault","Неизвестный сбой"}, {"Update failed","Ошибка обновления"}, {"(bad signature or","(плохая подпись или"}, {"network error)","ошибка сети)"},
    {"Update UEFI theme","Обновить тему UEFI"}, {"found","найдено"}, {"Uptime:","Время работы:"}, {"Uptime","Время работы"}, {"not connected","не подключено"},
    {"USB drive","USB-диск"}, {"USB error","Ошибка USB"}, {"USB flash/monitor is off meanwhile","USB-прошивка/монитор пока выкл."},
    {"UTC offset (-12..12):","Смещение UTC (-12..12):"}, {"Vendor:","Произв.:"}, {"Verified & written!","Проверено и записано!"},
    {"Activate via GRUB ->","Включите через GRUB ->"}, {"Voice pack download","Загрузка гол. пакета"}, {"Voice pack is","Голосовой пакет"}, {"already loaded","уже загружен"},
    {"Voice pack","Голосовой пакет"}, {"started","начата"}, {"Volume:","Громкость:"}, {"Waiting for the PC ...","Ожидание ПК..."}, {"WALL: LOCK","ОБОИ: БЛОК."},
    {"Wall: Lock","Обои: блок."}, {"WALL: MAIN","ОБОИ: ГЛАВН."}, {"Wall: Main","Обои: глав."}, {"WALL: SYSTEM","ОБОИ: СИСТ."}, {"Wall: System","Обои: сист."},
    {"Warning: 80MHz may","Внимание: 80 МГц может"}, {"cause WiFi/BT issues","вызвать сбои WiFi/BT"}, {"WiFi module disabled","Модуль WiFi выключен"},
    {"WiFi module","Модуль WiFi"}, {"WIFI NETWORKS","СЕТИ WIFI"}, {"WiFi networks wiped","Сети WiFi удалены"}, {"WiFi not connected","WiFi не подключён"},
    {"WiFi radio:","Радио WiFi:"}, {"WiFi status","Статус WiFi"}, {"WiFi connected","WiFi подключён"}, {"WiFi failed","Ошибка WiFi"}, {"Wipe WiFi networks","Удалить сети WiFi"},
    {"Wrong password","Неверный пароль"}, {"or cancelled","или отмена"}, {"XMC flash: keep XMC support ON","XMC флеш: оставьте XMC ВКЛ"},
    {"XMC support not needed","Поддержка XMC не нужна"}, {"N/A","н/д"}, {"n/a","н/д"},
    /* single words and menu names */
    {"Battery","Батарея"}, {"Settings","Настройки"}, {"Tools","Инструменты"}, {"Games","Игры"}, {"Apps","Приложения"}, {"Clock","Часы"}, {"Browser","Браузер"},
    {"Messenger","Мессенджер"}, {"Interpreter","Интерпретатор"}, {"System info","О системе"}, {"Logs","Журнал"}, {"Notifications","Уведомления"},
    {"Terminal","Терминал"}, {"Files","Файлы"}, {"Music Player","Плеер"}, {"Theme","Тема"}, {"Volume","Громкость"}, {"Sound","Звук"}, {"Rotation","Поворот"},
    {"Language","Язык"}, {"Back","Назад"}, {"Exit","Выход"}, {"Cancel","Отмена"}, {"Reboot","Перезагрузка"}, {"Refresh","Обновить"}, {"Status","Статус"},
    {"ENABLED","ВКЛЮЧЕНО"}, {"DISABLED","ВЫКЛЮЧЕНО"}, {"enabled","включено"}, {"disabled","выключено"}, {"Enabled","Включено"}, {"Disabled","Выключено"},
    {"ON","ВКЛ"}, {"OFF","ВЫКЛ"}, {"On","Вкл"}, {"Off","Выкл"}, {"Yes","Да"}, {"No","Нет"}, {"Wrong","Неверно"},
};
static const int K_N = (int)(sizeof(k_ph) / sizeof(k_ph[0]));
static uint16_t s_order[560];
static uint16_t s_start[129];
static bool s_ready = false;

static void build(void) {
    int n = K_N > 560 ? 560 : K_N;
    for (int i = 0; i < n; i++) s_order[i] = (uint16_t)i;
    std::sort(s_order, s_order + n, [](uint16_t a, uint16_t b) {
        unsigned char ca = (unsigned char)k_ph[a].en[0], cb = (unsigned char)k_ph[b].en[0];
        if (ca != cb) return ca < cb;
        return strlen(k_ph[a].en) > strlen(k_ph[b].en);          // longest phrase first
    });
    int p = 0;
    for (int c = 0; c < 128; c++) {
        s_start[c] = (uint16_t)p;
        while (p < n && (unsigned char)k_ph[s_order[p]].en[0] == c) p++;
    }
    s_start[128] = (uint16_t)n;
    s_ready = true;
}

static inline bool isw(unsigned char c) { return isalnum(c) || c >= 0x80; }

// returns the new length, or 0 when nothing was translated (the caller then prints the original)
static size_t tr_hook(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    if (n == 0 || !k85_lang_ru()) return 0;
    if (!s_ready) build();
    size_t o = 0;
    bool changed = false;
    for (size_t i = 0; i < n;) {
        unsigned char c = in[i];
        bool hit = false;
        if (c < 128) {
            for (int k = s_start[c]; k < s_start[c + 1]; k++) {
                const Ph &p = k_ph[s_order[k]];
                size_t L = strlen(p.en);
                if (i + L > n || memcmp(in + i, p.en, L) != 0) continue;
                if (isalnum((unsigned char)p.en[0]) && i > 0 && isw(in[i - 1])) continue;          // start of a word
                if (isalnum((unsigned char)p.en[L - 1]) && i + L < n && isw(in[i + L])) continue;   // end of a word
                size_t R = strlen(p.ru);
                if (o + R + 1 > cap) return 0;
                memcpy(out + o, p.ru, R);
                o += R;
                i += L;
                changed = true;
                hit = true;
                break;
            }
        }
        if (!hit) {
            if (o + 1 > cap) return 0;
            out[o++] = c;
            i++;
        }
    }
    return changed ? o : 0;
}

void k85_tr_install(void) { lgfx::lgfx_text_hook = tr_hook; }