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
 * Бутскрин (show_boot_screen): логотип 128×64 1:1 как на OLED, без масштабирования.
 * Вертикаль: центр в зоне над подписью версии; горизонт: по центру кадра.
 */
constexpr int kBootFooterReservedPx = 24;   /* строка v… + нижние поля */
constexpr int kBootLogoMinTopPx = 4;
constexpr int kBootVersionBottomPx = 6;     /* от низа кадра до baseline (size 1 ≈ 8px высоты) */
constexpr int kBootVersionRightPx = 6;

/** Init progress: якоря по сетке (title 4px, трек 32px, статус 52px, hint снизу). */
constexpr int kInitProgressTitleY = 4;
constexpr int kInitProgressTrackY = 32;
constexpr int kInitProgressStatusY = 52;
/** Baseline подсказки init_hint (size 1 ≈ 8px). */
constexpr int16_t kInitProgressHintBaselineY = kScreenH - 14;

/** Меню-список: вертикальная компоновка как на OLED (заголовок + строки). */
constexpr int kMenuTitleBarPx = 14;
constexpr int kMenuRowPx = 12;
/** Опциональные иконки 8x8 для будущих экранов (в 1:1 режиме Home рендерится вертикальным списком). */
constexpr int kHomeIconBitmapPx = 8;
constexpr int kHomeIconStripH = 28;
/** Legacy для совместимости; в текущем рендере не используется. */
constexpr int kMenuStripMinCellW = 18;
/** Печать в одну строку (~6 px/символ при size 1): заголовок и подпись под полосой. */
constexpr size_t kMenuTitlePrintChars = 38;
constexpr size_t kMenuLabelPrintChars = 38;

/** Подсказка внизу списка меню (высота полосы, Y курсора = kScreenH - это значение). */
constexpr int kMenuFooterHintH = 12;
constexpr int kMenuFooterHintY = kScreenH - kMenuFooterHintH;

/** Дашборд 4 строки: text size 2 (~12 px/символ) → ~20 символов на строку при ширине 240. */
constexpr int kDashTextSize = 2;
constexpr int kDashLinePx = 16;
constexpr size_t kDashCharsPerLine = 20;
constexpr int kDashStatusStripDefaultH = 135;

/** Полноэкранный текст: разделитель под заголовком (если заголовок пуст — якорь линии). */
constexpr int kFullscreenTitleSepY = 12;
constexpr int kFullscreenBodyStartY = 16;
constexpr int kFullscreenBodyLineStepPx = 10;
constexpr int kFullscreenTitleLineStepPx = 10;
constexpr int kFullscreenTitleMaxLines = 2;
constexpr int kFullscreenBottomMarginPx = 8;
constexpr int kFullscreenBodyMaxY = kScreenH - kFullscreenBottomMarginPx;
/** Default font ~6 px wide → ~40 символов в строке 240 px. */
constexpr size_t kFullscreenCharsPerLine = 40;
constexpr size_t kFullscreenSnapTitleMax = 64;
constexpr size_t kFullscreenSnapBodyMax = 480;

#if defined(RIFTLINK_BOARD_HELTEC_T114)
/** Зарезервировано под фазы 1+; сейчас пусто (init дисплея в display_nrf::init). */
void init();
#endif

}  // namespace ui_t114
