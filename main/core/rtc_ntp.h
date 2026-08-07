#pragma once
#include <stdbool.h>

void k85_rtc_ntp_init(void);          // РІС‹СЃС‚Р°РІР»СЏРµС‚ TZ (MSK-3), РІС‹Р·С‹РІР°С‚СЊ СЂР°Р· РїСЂРё СЃС‚Р°СЂС‚Рµ
void k85_rtc_apply_tz(int utc_offset); // применить смещение (-12..+12) сразу
void k85_rtc_set_manual_time(int hour, int min, int sec);
void k85_rtc_set_manual_date(int day, int month, int year); // year полный, напр. 2026 // выставить время вручную
bool k85_ntp_sync_now(void);          // Р±Р»РѕРєРёСЂСѓСЋС‰Р°СЏ РїРѕРїС‹С‚РєР° СЃРёРЅС…СЂРѕРЅРёР·Р°С†РёРё (5СЃ С‚Р°Р№РјР°СѓС‚)
bool k85_is_ntp_synced(void);

// РђРЅР°Р»РѕРіРё get_time_str()/get_date_str()/get_uptime_str() РёР· MicroPython.
// Р’РѕР·РІСЂР°С‰Р°СЋС‚ СѓРєР°Р·Р°С‚РµР»СЊ РЅР° РІРЅСѓС‚СЂРµРЅРЅРёР№ СЃС‚Р°С‚РёС‡РµСЃРєРёР№ Р±СѓС„РµСЂ (РІР°Р»РёРґРµРЅ РґРѕ СЃР»РµРґСѓСЋС‰РµРіРѕ РІС‹Р·РѕРІР°).
const char *k85_get_time_str(void);
const char *k85_get_date_str(void);
const char *k85_get_uptime_str(void);


bool k85_rtc_is_present(void); // true если RTC-чип отвечает


