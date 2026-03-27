/**
 * Политика размеров текста по платформам (согласована с ui_layout_profile).
 * На OLED крупный шрифт — только точечно (мало строк); на T‑Pager/Paper — базово 1.
 *
 * Базовый шрифт Adafruit GFX при setTextSize(1): глиф 8×8 px — для центрирования в строке
 * menuListRowHeight см. ui_layout::Profile::menuListTextOffsetY / menuListIconTopOffsetY.
 */
#pragma once

namespace ui_typography {

/** OLED 128×64: size 2 съедает половину строк; базовый UI остаётся size 1, воздух — за счёт row step / gaps. */
inline int bodyTextSizeOled() { return 1; }
inline int bodyTextSizeTpager() { return 1; }
inline int bodyTextSizePaper() { return 1; }

/** Режим «крупное чтение» на OLED — при необходимости отдельный экран (setTextSize(2)). */
inline int readingTextSizeOled() { return 2; }

/** Заголовок бутскрина T‑Pager (крупная надпись). */
inline int bootTitleTextSizeTpager() { return 2; }

/** Версия на бутскрине Paper (под битмапом). */
inline int bootVersionTextSizePaper() { return 2; }

/** Вкладка «Узел»: базовый size 1 (между 1 и 2 в GFX нет дробного масштаба; size 2 оказался слишком крупным). */
inline int nodeTabTextSizeOled() { return 1; }
inline int nodeTabTextSizeTpager() { return 1; }
/** Шаг строк на «Узел»: чуть больше SUBMENU/16 px — читаемость без size 2. */
inline int nodeMsgLineStepOled() { return 11; }
inline int nodeMsgLineStepTpager() { return 19; }
/** Вкладка Msg на T‑Pager — шаг как в списках. */
inline int msgBodyLineStepTpager() { return 16; }

}  // namespace ui_typography
