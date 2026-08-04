
## v4.3 (CPLUSPLUS-RELEASE) - 2026-08-04

### WiFi Hotspot
- Пароль точки доступа показывается на экране устройства (строка PW под SSID).
- Пароль добавлен в карточку статуса веб-интерфейса (http://192.168.4.1).
- AP: SSID k85OS-XXXX (виден на экране), пароль 12345678.

### Прочие изменения в сборке
- IR Remote / Music Player / меню и загрузочный экран - дополни по факту

### Прошивка (ESP32-S3, 8 MB, dio, 80 MHz)
python -m esptool --chip esp32s3 -b 460800 write_flash 0x0 k85OS_4.3-CPLUSPLUS-RELEASE.bin
