/**
 * RiftLink Locale — язык интерфейса
 * en, ru. ESP: NVS. nRF: riftlink_kv (InternalFS).
 */

#pragma once

#define LANG_EN 0
#define LANG_RU 1

namespace locale {

void init();
bool isSet();
int getLang();
bool setLang(int lang);
const char* get(const char* key);
/** Для OLED: ASCII-транслит (кириллица не поддерживается шрифтом) */
const char* getForDisplay(const char* key);
/** Строка для отображения для указанного языка (en или ru_oled) */
const char* getForLang(const char* key, int lang);
const char* getLangName(int lang);

}  // namespace locale
