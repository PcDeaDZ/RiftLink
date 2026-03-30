/**
 * Профиль локального UI для Heltec Mesh Node T114 (ST7789 135×240).
 * Эпик: docs/plans/T114_UI_FULL_PORT_EPIC.md — фаза 0 (единые константы, ориентация).
 * Подключать только при RIFTLINK_BOARD_HELTEC_T114.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace ui_t114 {

/** Физические размеры панели 1.14" — только для Adafruit_ST7789::init (ветка 135×240 в драйвере). */
constexpr int16_t kPanelW = 135;
constexpr int16_t kPanelH = 240;

/**
 * Логическая ширина/высота кадра после init(kPanelW,kPanelH) и setRotation(kGfxRotation).
 * kGfxRotation = 1 — альбом (ширина 240, высота 135), меню «вдоль длинной стороны».
 */
constexpr int16_t kScreenW = 240;
constexpr int16_t kScreenH = 135;

/**
 * setRotation для Adafruit ST7789: 1 — альбом 240×135; 0 — портрет 135×240.
 * Менять только осознанно: вся вёрстка в display_nrf завязана на kScreenW/kScreenH.
 */
constexpr uint8_t kGfxRotation = 1;

/** Запись в ST7789 (см. Meshtastic SPI_FREQUENCY 40 MHz). */
constexpr uint32_t kSpiWriteHz = 40000000u;

/** Визуальная сетка (отступы и якоря кратны этому значению). */
constexpr int kGridPx = 8;

/** Отступы контента от краёв (пиксели). */
constexpr int16_t kMarginX = 0;
constexpr int16_t kMarginY = 2;

/**
 * Бутскрин (show_boot_screen): логотип 128×64 из ассета.
 * Желаемый масштаб — до kBootLogoScale; фактический = min(kBootLogoScale, по ширине экрана и высоте зоны над подвалом),
 * чтобы картинка целиком помещалась без обрезки (на 240×135 «честные» 2× по обеим осям не влезают).
 */
constexpr int kBootLogoScale = 2;
constexpr uint8_t kBootVersionTextSize = 2;
/**
 * Нижняя полоса под строку «v…» (text size kBootVersionTextSize), без пересечения с логотипом.
 * Меньше значение — выше logoAreaH и крупнее логотип (до min(2×, 240/128, logoAreaH/64)).
 * На Wireless Paper / E-Ink другое разрешение — там картинка крупнее «естественно»; на T114 240×135
 * упираемся в высоту, поэтому резерв держим минимально достаточным.
 */
constexpr int kBootFooterReservedPx = 26;
constexpr int kBootLogoMinTopPx = 2;
/** Отступ от нижнего края экрана до baseline строки «v…» (меньше — текст ниже). */
constexpr int kBootVersionBottomPx = 2;
constexpr int kBootVersionLeftPx = 6;       /* отступ версии от левого края */
/** Сколько держать бутскрин с логотипом до перехода к мастеру языка / init progress (мс). */
constexpr uint32_t kBootSplashHoldMs = 1200u;

/**
 * Init progress: крупный режим (~2× от базового size 1) — заголовок, трек, статус, подсказка.
 * Ширина строки при text size 2: ~6×2 px на символ → ~20 символов на 240 px.
 */
constexpr uint8_t kInitProgressTextSize = 2;
constexpr int kInitProgressTrackR = 6;
/** Горизонтальный запас под круги шагов (см. расчёт pitch в display_nrf). */
constexpr int kInitProgressTrackMarginX = 48;
constexpr int kInitProgressPitchMin = 24;
constexpr int kInitProgressPitchMax = 44;
constexpr size_t kInitProgressCharsPerLine = 20;
constexpr int kInitProgressTitleY = 4;
constexpr int kInitProgressTrackY = 38;
constexpr int kInitProgressStatusY = 58;
/** Высота полосы под строку статуса (дельта-очистка + size 2). */
constexpr int kInitProgressStatusBandH = 22;
/** Baseline подсказки init_hint (text size kInitProgressTextSize). */
constexpr int16_t kInitProgressHintBaselineY = kScreenH - 20;

/**
 * Единый setTextSize для дашборда, вкладок (fullscreen), меню и самотеста.
 * При смене — подстроить kDashLinePx и шаги полноэкранной вёрстки (ниже).
 */
constexpr uint8_t kDashTextSize = 2;
/** ~40 символов при size 1 на 240px; при size N — деление по ширине глифа. */
constexpr size_t kUiCharsPerLine = 40u / (size_t)kDashTextSize;

/** Меню-список: вертикальная компоновка (заголовок + строки). Высоты под kDashTextSize. */
constexpr int kMenuTitleBarPx = 8 + 7 * (int)kDashTextSize;
constexpr int kMenuRowPx = 6 + 8 * (int)kDashTextSize;
/** Опциональные иконки 8x8 для будущих экранов (в 1:1 режиме Home рендерится вертикальным списком). */
constexpr int kHomeIconBitmapPx = 8;
constexpr int kHomeIconStripH = 28;
/** Как display.cpp ICON_LABEL_GAP_OLED + отступ слева: иконка 8×8, текст справа. */
constexpr int kMenuIconLeftPx = 4;
constexpr int kMenuIconToTextGapPx = 6;
constexpr int kMenuTextStartXWithIcon = kMenuIconLeftPx + kHomeIconBitmapPx + kMenuIconToTextGapPx;
/** Меньше kMenuLabelPrintChars — место под иконку (size 2 → ~12 px/символ). */
constexpr size_t kMenuLabelPrintCharsWithIcon =
    (size_t)((kScreenW - kMenuTextStartXWithIcon) / (6 * (int)kDashTextSize));
/** Сводка SYS (6 строк, size 1): как плотность строк на OLED V3 в drawContentSys. */
constexpr int kMenuSysBrowseRowPx = 12;
/** Legacy для совместимости; в текущем рендере не используется. */
constexpr int kMenuStripMinCellW = 18;
/** Печать в одну строку (clipped по ширине экрана при текущем kDashTextSize). */
constexpr size_t kMenuTitlePrintChars = kUiCharsPerLine;
constexpr size_t kMenuLabelPrintChars = kUiCharsPerLine;

/** Подсказка внизу списка меню (высота полосы, Y курсора = kScreenH - это значение). */
constexpr int kMenuFooterHintH = 12 * (int)kDashTextSize;
constexpr int kMenuFooterHintY = kScreenH - kMenuFooterHintH;

/**
 * Дашборд: 4 строки; вертикальные якоря в display_nrf (с учётом таб-бара).
 * Шаг строки ≈ высота глифа при kDashTextSize.
 */
constexpr int kDashLeftMargin = 4;
constexpr int kDashLinePx = 6 + 7 * (int)kDashTextSize;
/** Полоска иконок вкладок (таб-режим), как kHomeIconStripH. */
constexpr int kDashTabBarH = 28;
/**
 * Топбар статуса — та же высота и шкала, что у полоски вкладок (эстетика «одна полоса»).
 * Двойная линия внизу внутри полосы (как draw_dash_tab_strip).
 */
constexpr int kT114StatusBarBodyH = kDashTabBarH;
/** Смещение контента под топбаром (без отдельной «второй полосы» под линиями). */
constexpr int kT114ChromeStatusTotalH = kT114StatusBarBodyH;
constexpr int kDashBottomPadPx = 8;
constexpr size_t kDashCharsPerLine = kUiCharsPerLine;
constexpr int kDashStatusStripDefaultH = 135;

/** Полноэкранный текст: разделитель под заголовком (если заголовок пуст — якорь линии). */
constexpr int kFullscreenTitleSepY = 6 + 3 * (int)kDashTextSize;
constexpr int kFullscreenBodyStartY = 16;
constexpr int kFullscreenBodyLineStepPx = 10 * (int)kDashTextSize;
constexpr int kFullscreenTitleLineStepPx = 10 * (int)kDashTextSize;
constexpr int kFullscreenTitleMaxLines = 2;
constexpr int kFullscreenBottomMarginPx = 8;
constexpr int kFullscreenBodyMaxY = kScreenH - kFullscreenBottomMarginPx;
constexpr size_t kFullscreenCharsPerLine = kUiCharsPerLine;
constexpr size_t kFullscreenSnapTitleMax = 64;
constexpr size_t kFullscreenSnapBodyMax = 480;

#if defined(RIFTLINK_BOARD_HELTEC_T114)
/** Зарезервировано под фазы 1+; сейчас пусто (init дисплея в display_nrf::init). */
void init();
#endif

}  // namespace ui_t114
