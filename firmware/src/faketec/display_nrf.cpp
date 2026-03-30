/**
 * Дисплей nRF: Heltec T114 — встроенный ST7789 (SPI1); иначе SSD1306 I2C.
 */

#include "display_nrf.h"
#include "board_pins.h"
#include "ble/ble.h"
#include "locale/locale.h"
#include "version.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "bootscreen_oled.h"
#include "cp1251_to_rusfont.h"
#include "ui_t114.h"
#include "ui/ui_display_prefs.h"

namespace display_nrf {

namespace {

Adafruit_ST7789 g_tft(&SPI1, TFT_SPI_CS, TFT_SPI_DC, TFT_SPI_RST);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

/** Режим отрисовки — чтобы не делать fillScreen при каждом обновлении дашборда (как дельта в Meshtastic TFT). */
enum class ScreenKind : uint8_t { None, Boot, Status4, MenuFull, FullscreenText, InitProgress, Selftest, LastMsgPoll };
static ScreenKind g_screen_kind = ScreenKind::None;

/** Кэш списка меню — для перерисовки только двух строк при смене выделения (без чёрной вспышки на весь TFT). */
static const char* const* s_menu_labels_arr = nullptr;
static const char* s_menu_footer_ptr = nullptr;
static char s_menu_title_snap[64] = "";
static int s_menu_count = -1;
static int s_menu_scroll = -1;
static int s_menu_sel = -1;

/** Кэш полноэкранного текста: без повторной чёрной заливки при том же title/body (смена языка/двойной вызов). */
static char s_fs_title_snap[64];
static char s_fs_body_snap[480];
static bool s_fs_snap_valid = false;

/** Кэш 4 строк дашборда (text size 2): без повторной заливки при том же содержимом (ложные клики / двойные вызовы). */
static char s_status4_lines[4][48];
static bool s_status4_valid = false;

static bool status4_all_single_row(const char lines[4][48]) {
  for (int i = 0; i < 4; i++) {
    if (strlen(lines[i]) > ui_t114::kDashCharsPerLine) return false;
  }
  return true;
}

/** CP1251 → индексы glcdfont (patches/glcdfont.c), как на ESP в display.cpp. */
static void tft_print_cp1251(const char* s) {
  if (!s) return;
  for (const char* p = s; *p; ++p) {
    const unsigned char c = cp1251_to_rusfont((unsigned char)*p);
    g_tft.print((char)c);
  }
}

static void tft_println_cp1251(const char* s) {
  tft_print_cp1251(s);
  g_tft.println();
}

static void tft_print_clipped(const char* s, size_t maxPrint) {
  if (!s) return;
  char conv[256];
  size_t j = 0;
  for (size_t i = 0; s[i] && j < sizeof(conv) - 1; i++) {
    conv[j++] = (char)cp1251_to_rusfont((unsigned char)s[i]);
  }
  conv[j] = 0;
  const size_t n = strlen(conv);
  if (n <= maxPrint) {
    g_tft.print(conv);
    return;
  }
  if (maxPrint <= 2) {
    g_tft.print("..");
    return;
  }
  const size_t keep = maxPrint - 2U;
  char buf[28];
  size_t k = keep;
  if (k > sizeof(buf) - 4U) k = sizeof(buf) - 4U;
  memcpy(buf, conv, k);
  buf[k] = '.';
  buf[k + 1] = '.';
  buf[k + 2] = 0;
  g_tft.print(buf);
}

/**
 * Оценка высоты области дашборда (text size 2, ~12px ширина символа → ~20 символов в строке 240px).
 * Чем меньше чёрная полоса перед текстом при Status4→Status4, тем слабее «вспышка» на весь верх TFT.
 */
static int16_t status4_estimated_clear_height(const char lines[4][48]) {
  int total = 0;
  for (int i = 0; i < 4; i++) {
    const size_t n = strlen(lines[i]);
    if (n == 0) continue;
    const int rows =
        (int)((n + ui_t114::kDashCharsPerLine - 1) / ui_t114::kDashCharsPerLine);
    total += rows * ui_t114::kDashLinePx;
  }
  if (total < 64) total = 64;
  if (total > (int)ui_t114::kScreenH) total = (int)ui_t114::kScreenH;
  total += 8;
  if (total > (int)ui_t114::kScreenH) total = (int)ui_t114::kScreenH;
  return (int16_t)total;
}

/** Частичная заливка только при подряд двух Status4; иначе полный кадр (смена меню/формы). */
static void clear_for_screen(ScreenKind next, int16_t status4_dash_strip_h = -1) {
  if (next != ScreenKind::FullscreenText) {
    s_fs_snap_valid = false;
  }
  if (next != ScreenKind::MenuFull) {
    s_menu_labels_arr = nullptr;
  }
  if (next != ScreenKind::Status4) {
    s_status4_valid = false;
  }
  const bool dash_refresh = (g_screen_kind == ScreenKind::Status4 && next == ScreenKind::Status4);
  if (dash_refresh) {
    int16_t h = ui_t114::kDashStatusStripDefaultH;
    if (status4_dash_strip_h > 0) {
      h = status4_dash_strip_h;
      if (h > ui_t114::kScreenH) h = ui_t114::kScreenH;
    }
    g_tft.fillRect(0, 0, ui_t114::kScreenW, h, ST77XX_BLACK);
  } else {
    g_tft.fillRect(0, 0, ui_t114::kScreenW, ui_t114::kScreenH, ST77XX_BLACK);
  }
  g_screen_kind = next;
}

/** Заголовок полноэкранного режима: до двух строк, перенос по словам (длинные «Home > …»). */
static int16_t draw_fullscreen_title_wrapped(int16_t y0, const char* title) {
  if (!title || !title[0]) return y0;
  const int lineH = ui_t114::kFullscreenTitleLineStepPx;
  const size_t maxCol = ui_t114::kFullscreenCharsPerLine;
  int y = (int)y0;
  const char* seg = title;
  size_t rem = strlen(title);
  int lines_out = 0;
  while (rem > 0 && lines_out < ui_t114::kFullscreenTitleMaxLines) {
    while (rem > 0 && *seg == ' ') {
      seg++;
      rem--;
    }
    if (rem == 0) break;
    size_t take = rem;
    if (take > maxCol) {
      size_t br = maxCol;
      while (br > 0 && seg[br - 1] != ' ') br--;
      if (br == 0)
        take = maxCol;
      else
        take = br;
    }
    size_t n = take;
    while (n > 0 && seg[n - 1] == ' ') n--;
    char lineBuf[32];
    if (n > sizeof(lineBuf) - 1U) n = sizeof(lineBuf) - 1U;
    memcpy(lineBuf, seg, n);
    lineBuf[n] = 0;
    for (size_t k = 0; lineBuf[k]; k++) lineBuf[k] = (char)cp1251_to_rusfont((unsigned char)lineBuf[k]);
    g_tft.setCursor(0, y);
    g_tft.print(lineBuf);
    y += lineH;
    lines_out++;
    seg += take;
    rem -= take;
  }
  return (int16_t)y;
}

/** Тело полноэкранного режима: явные \n + перенос по словам под ширину T114. */
static void draw_fullscreen_body_wrapped(int16_t y0, int16_t yMax, const char* body) {
  if (!body || !body[0]) return;
  const int lineH = ui_t114::kFullscreenBodyLineStepPx;
  const size_t maxCol = ui_t114::kFullscreenCharsPerLine;
  int y = (int)y0;
  const char* p = body;
  while (*p && y < (int)yMax) {
    if (*p == '\n') {
      p++;
      y += lineH;
      continue;
    }
    const char* nl = strchr(p, '\n');
    const size_t segLen = nl ? (size_t)(nl - p) : strlen(p);
    const char* seg = p;
    size_t rem = segLen;
    while (rem > 0 && y < (int)yMax) {
      while (rem > 0 && *seg == ' ') {
        seg++;
        rem--;
      }
      if (rem == 0) break;
      size_t take = rem;
      if (take > maxCol) {
        size_t br = maxCol;
        while (br > 0 && seg[br - 1] != ' ') br--;
        if (br == 0)
          take = maxCol;
        else
          take = br;
      }
      size_t n = take;
      while (n > 0 && seg[n - 1] == ' ') n--;
      char lineBuf[32];
      if (n > sizeof(lineBuf) - 1U) n = sizeof(lineBuf) - 1U;
      memcpy(lineBuf, seg, n);
      lineBuf[n] = 0;
      for (size_t k = 0; lineBuf[k]; k++) lineBuf[k] = (char)cp1251_to_rusfont((unsigned char)lineBuf[k]);
      g_tft.setCursor(0, y);
      g_tft.print(lineBuf);
      y += lineH;
      seg += take;
      rem -= take;
    }
    p = nl ? nl + 1 : p + segLen;
  }
}

}  // namespace

void apply_rotation_from_prefs() {
  const uint8_t r = ui_display_prefs::getFlip180() ? 3u : ui_t114::kGfxRotation;
  g_tft.setRotation(r);
}

bool init() {
  g_ok = false;
  // Heltec T114: VTFT_CTRL включает питание матрицы (Meshtastic: LOW = on). Иначе подсветка/шина могут «моргать», картинки нет.
  pinMode(TFT_VTFT_CTRL, OUTPUT);
  digitalWrite(TFT_VTFT_CTRL, TFT_VTFT_PWR_ON);
  // Чуть дольше, чем 20 ms: стабилизация питания матрицы до SPI (Meshtastic поднимает VTFT в DISPLAYON после init драйвера).
  delay(50);
  pinMode(TFT_BL, OUTPUT);
  // Подсветка после init (как в туториалах Adafruit): не показывать мусор на шине до первого чёрного кадра.
  digitalWrite(TFT_BL, TFT_BL_ON == LOW ? HIGH : LOW);
  delay(5);
  SPI1.setPins(TFT_SPI_MISO, TFT_SPI_SCK, TFT_SPI_MOSI);
  SPI1.begin();
  // 1.14" ST7789: init(135,240) — ветка панели в Adafruit; логический кадр — kScreenW×kScreenH после setRotation.
  // SPI_MODE0 — как LovyanGFX cfg.spi_mode = 0 в Meshtastic TFTDisplay (ST7789).
  g_tft.init(ui_t114::kPanelW, ui_t114::kPanelH, SPI_MODE0);
  apply_rotation_from_prefs();
  g_tft.setSPISpeed(ui_t114::kSpiWriteHz);
  // Белый на чёрном — перерисовка символов без «просветов»; ниже кадры батчим startWrite/endWrite (меньше артефактов на ST7789).
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextWrap(false);
  /** Без cp437(true) Adafruit_GFX сдвигает коды >=176 — ломает кириллицу в rusfont (как OLED в display.cpp). */
  g_tft.cp437(true);
  g_tft.startWrite();
  g_tft.fillRect(0, 0, ui_t114::kScreenW, ui_t114::kScreenH, ST77XX_BLACK);
  g_tft.endWrite();
  digitalWrite(TFT_BL, TFT_BL_ON);
  g_tft.setCursor(0, 0);
  g_screen_kind = ScreenKind::None;
  g_ok = true;
  ui_t114::init();
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Boot);
  g_tft.setCursor(0, 0);
  if (line1) tft_println_cp1251(line1);
  if (line2) tft_println_cp1251(line2);
  g_tft.endWrite();
}

void show_boot_screen() {
  if (!g_ok) return;
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Boot);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  const int bx = ((int)ui_t114::kScreenW - (int)BOOTSCREEN_OLED_W) / 2;
  const int logoAreaH = (int)ui_t114::kScreenH - ui_t114::kBootFooterReservedPx;
  int by = (logoAreaH > (int)BOOTSCREEN_OLED_H) ? ((logoAreaH - (int)BOOTSCREEN_OLED_H) / 2) : 0;
  if (by < (int)ui_t114::kBootLogoMinTopPx) by = (int)ui_t114::kBootLogoMinTopPx;
  if (bx >= 0 && by >= 0) {
    g_tft.drawBitmap(bx, by, bootscreen_oled, BOOTSCREEN_OLED_W, BOOTSCREEN_OLED_H, ST77XX_WHITE);
  }
  g_tft.setTextSize(1);
  char ver[20];
  snprintf(ver, sizeof(ver), "v%s", RIFTLINK_VERSION);
  int16_t x1, y1;
  uint16_t tw, th;
  g_tft.getTextBounds(ver, 0, 0, &x1, &y1, &tw, &th);
  const int16_t verY = (int16_t)((int)ui_t114::kScreenH - (int)th - (int)ui_t114::kBootVersionBottomPx);
  const int16_t verX = (int16_t)((int)ui_t114::kScreenW - (int)tw - (int)ui_t114::kBootVersionRightPx);
  g_tft.setCursor(verX, verY);
  g_tft.print(ver);
  g_tft.endWrite();
  g_screen_kind = ScreenKind::Boot;
}

void show_init_progress(int doneCount, int totalSteps, const char* statusLine) {
  if (!g_ok) return;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  g_tft.startWrite();
  clear_for_screen(ScreenKind::InitProgress);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, ui_t114::kInitProgressTitleY);
  tft_print_cp1251(locale::getForDisplay("init_title"));
  const int cy = ui_t114::kInitProgressTrackY;
  const int r = 3;
  const int n = totalSteps;
  int pitch = n > 1 ? (ui_t114::kScreenW - 24) / (n - 1) : 0;
  if (pitch < 12) pitch = 12;
  if (pitch > 22) pitch = 22;
  const int startX = (int)ui_t114::kScreenW / 2 - ((n - 1) * pitch) / 2;
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitch;
    const int cx1 = startX + (i + 1) * pitch;
    g_tft.drawLine(cx0 + r, cy, cx1 - r, cy, ST77XX_WHITE);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitch;
    if (i < doneCount) {
      g_tft.fillCircle(cx, cy, r, ST77XX_WHITE);
    } else if (i == doneCount && doneCount < totalSteps) {
      g_tft.drawCircle(cx, cy, r, ST77XX_WHITE);
      g_tft.fillCircle(cx, cy, 1, ST77XX_WHITE);
    } else {
      g_tft.drawCircle(cx, cy, r, ST77XX_WHITE);
    }
  }
  if (statusLine && statusLine[0]) {
    g_tft.setCursor(0, ui_t114::kInitProgressStatusY);
    tft_print_clipped(statusLine, ui_t114::kFullscreenCharsPerLine);
  }
  g_tft.setCursor(0, ui_t114::kInitProgressHintBaselineY);
  tft_print_clipped(locale::getForDisplay("init_hint"), ui_t114::kFullscreenCharsPerLine);
  g_tft.endWrite();
}

void show_warning_blocking(const char* line1, const char* line2, uint32_t durationMs) {
  if (!g_ok) return;
  char body[220];
  snprintf(body, sizeof(body), "%s\n%s", line1 ? line1 : "", line2 ? line2 : "");
  show_fullscreen_text(locale::getForDisplay("warn_title"), body);
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < durationMs) {
    ble::update();
    delay(50);
  }
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Selftest);
  g_tft.setCursor(0, 0);
  tft_println_cp1251(locale::getForDisplay("menu_selftest"));
  tft_println_cp1251(radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  tft_println_cp1251(antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  tft_print_cp1251(locale::getForDisplay("battery"));
  g_tft.printf(" %umV\n", (unsigned)batteryMv);
  tft_print_cp1251(locale::getForDisplay("selftest_heap"));
  g_tft.printf(" %u\n", (unsigned)heapFree);
  g_tft.endWrite();
}

void queue_last_msg(const char* fromHex, const char* text) {
  if (!g_ok) return;
  char nextFrom[20] = "";
  char nextText[48] = "";
  if (fromHex) {
    strncpy(nextFrom, fromHex, sizeof(nextFrom) - 1);
    nextFrom[sizeof(nextFrom) - 1] = 0;
  }
  if (text) {
    strncpy(nextText, text, sizeof(nextText) - 1);
    nextText[sizeof(nextText) - 1] = 0;
  }
  if (strcmp(nextFrom, g_line_from) == 0 && strcmp(nextText, g_line_text) == 0) return;
  memcpy(g_line_from, nextFrom, sizeof(g_line_from));
  memcpy(g_line_text, nextText, sizeof(g_line_text));
  g_last_dirty = true;
}

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4) {
  if (!g_ok) return;
  const char* src[4] = {line1, line2, line3, line4};
  char next4[4][48];
  for (int i = 0; i < 4; i++) {
    if (src[i]) {
      strncpy(next4[i], src[i], sizeof(next4[i]) - 1);
      next4[i][sizeof(next4[i]) - 1] = 0;
    } else {
      next4[i][0] = 0;
    }
  }
  if (g_screen_kind == ScreenKind::Status4 && s_status4_valid) {
    bool same = true;
    for (int i = 0; i < 4; i++) {
      if (strcmp(next4[i], s_status4_lines[i]) != 0) {
        same = false;
        break;
      }
    }
    if (same) return;
  }

  // Как буферный кадр в Meshtastic: обновляем только изменённые строки без чёрной заливки всей полосы (нет «вспышки»).
  if (g_screen_kind == ScreenKind::Status4 && s_status4_valid && status4_all_single_row(next4) &&
      status4_all_single_row(s_status4_lines)) {
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    g_tft.setTextSize(ui_t114::kDashTextSize);
    g_tft.setTextWrap(false);
    g_tft.startWrite();
    for (int i = 0; i < 4; i++) {
      if (strcmp(next4[i], s_status4_lines[i]) == 0) continue;
      const int16_t y = (int16_t)(i * ui_t114::kDashLinePx);
      g_tft.fillRect(0, y, ui_t114::kScreenW, ui_t114::kDashLinePx, ST77XX_BLACK);
      g_tft.setCursor(0, y);
      tft_print_cp1251(next4[i]);
    }
    g_tft.endWrite();
    memcpy(s_status4_lines, next4, sizeof(next4));
    s_status4_valid = true;
    g_screen_kind = ScreenKind::Status4;
    return;
  }

  int16_t dash_strip_h = -1;
  if (g_screen_kind == ScreenKind::Status4) {
    dash_strip_h = status4_estimated_clear_height(next4);
  }

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Status4, dash_strip_h);
  g_tft.setCursor(0, 0);
  if (line1) tft_println_cp1251(line1);
  if (line2) tft_println_cp1251(line2);
  if (line3) tft_println_cp1251(line3);
  if (line4) tft_println_cp1251(line4);
  g_tft.endWrite();
  memcpy(s_status4_lines, next4, sizeof(next4));
  s_status4_valid = true;
}

/** Не вызывается из main loop: полный fillScreen здесь перезаписывал дашборд и давал моргание на T114 ST7789. */
void poll() {
  if (!g_ok || !g_last_dirty) return;
  uint32_t now = millis();
  // Реже полный кадр — при активном mesh меньше вспышек чёрного.
  if ((uint32_t)(now - g_last_poll_ms) < 750) return;
  g_last_poll_ms = now;

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::LastMsgPoll);
  g_tft.setCursor(0, 0);
  tft_println_cp1251(locale::getForDisplay("last_msg_title"));
  if (g_line_from[0]) tft_println_cp1251(g_line_from);
  if (g_line_text[0]) tft_println_cp1251(g_line_text);
  g_tft.endWrite();
  g_last_dirty = false;
}

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll,
    const char* footerHint, const uint8_t* const* icons) {
  if (!g_ok || !labels || count < 1) return;
  (void)icons;
  if (selected < 0) selected = 0;
  if (selected >= count) selected = count - 1;
  if (scroll < 0) scroll = 0;
  const int footerH = (footerHint && footerHint[0]) ? ui_t114::kMenuFooterHintH : 0;
  const int titleH = ui_t114::kMenuTitleBarPx;
  const int rowH = ui_t114::kMenuRowPx;
  int maxRows = ((int)ui_t114::kScreenH - titleH - footerH) / rowH;
  if (maxRows < 1) maxRows = 1;

  int scrollAdj = scroll;
  if (scrollAdj > selected) scrollAdj = selected;
  if (scrollAdj < selected - maxRows + 1) scrollAdj = selected - maxRows + 1;
  if (scrollAdj < 0) scrollAdj = 0;
  const int maxScroll = count - maxRows;
  if (maxScroll > 0 && scrollAdj > maxScroll) scrollAdj = maxScroll;

  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::MenuFull);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(ui_t114::kMarginX, ui_t114::kMarginY);
  if (title) tft_print_clipped(title, ui_t114::kMenuTitlePrintChars);
  g_tft.drawFastHLine(0, titleH - 1, ui_t114::kScreenW, ST77XX_WHITE);

  int y = titleH;
  for (int row = 0; row < maxRows; row++) {
    const int idx = scrollAdj + row;
    if (idx >= count) break;
    const bool sel = (idx == selected);
    if (sel) {
      g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_WHITE);
      g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    } else {
      g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_BLACK);
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
    g_tft.setCursor(2, y + 2);
    if (labels[idx]) tft_print_clipped(labels[idx], ui_t114::kMenuLabelPrintChars);
    y += rowH;
  }

  if (footerH > 0 && footerHint) {
    g_tft.fillRect(0, ui_t114::kMenuFooterHintY, ui_t114::kScreenW, footerH, ST77XX_BLACK);
    g_tft.setCursor(0, ui_t114::kMenuFooterHintY);
    tft_print_cp1251(footerHint);
  }
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.endWrite();

  if (title) {
    strncpy(s_menu_title_snap, title, sizeof(s_menu_title_snap) - 1);
    s_menu_title_snap[sizeof(s_menu_title_snap) - 1] = 0;
  } else {
    s_menu_title_snap[0] = 0;
  }
  s_menu_labels_arr = labels;
  s_menu_footer_ptr = footerHint;
  s_menu_count = count;
  s_menu_scroll = scrollAdj;
  s_menu_sel = selected;
  g_screen_kind = ScreenKind::MenuFull;
}

void show_home_menu_strip(const char* title, const char* const* labels, const uint8_t* const* icons, int count, int selected,
    int scroll, const char* footerHint) {
  show_menu_list(title, labels, count, selected, scroll, footerHint, icons);
}

void show_fullscreen_text(const char* title, const char* body) {
  if (!g_ok) return;
  char tbuf[64];
  char bbuf[480];
  if (title) {
    strncpy(tbuf, title, sizeof(tbuf) - 1);
    tbuf[sizeof(tbuf) - 1] = 0;
  } else {
    tbuf[0] = 0;
  }
  if (body) {
    strncpy(bbuf, body, sizeof(bbuf) - 1);
    bbuf[sizeof(bbuf) - 1] = 0;
  } else {
    bbuf[0] = 0;
  }
  if (s_fs_snap_valid && strcmp(tbuf, s_fs_title_snap) == 0 && strcmp(bbuf, s_fs_body_snap) == 0) return;

  g_tft.startWrite();
  clear_for_screen(ScreenKind::FullscreenText);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  int16_t y_sep;
  if (tbuf[0]) {
    const int16_t y_after = draw_fullscreen_title_wrapped(ui_t114::kMarginY, tbuf);
    y_sep = (int16_t)(y_after + 2);
  } else {
    y_sep = ui_t114::kFullscreenTitleSepY;
  }
  g_tft.drawFastHLine(0, y_sep, ui_t114::kScreenW, ST77XX_WHITE);
  const int16_t body_y0 = (int16_t)(y_sep + 4);
  const int16_t body_y_max = (int16_t)(ui_t114::kScreenH - ui_t114::kFullscreenBottomMarginPx);
  if (bbuf[0]) draw_fullscreen_body_wrapped(body_y0, body_y_max, bbuf);

  strncpy(s_fs_title_snap, tbuf, sizeof(s_fs_title_snap) - 1);
  s_fs_title_snap[sizeof(s_fs_title_snap) - 1] = 0;
  strncpy(s_fs_body_snap, bbuf, sizeof(s_fs_body_snap) - 1);
  s_fs_body_snap[sizeof(s_fs_body_snap) - 1] = 0;
  s_fs_snap_valid = true;
  g_tft.endWrite();
}

void get_last_msg_peek(char* fromBuf, size_t fromLen, char* textBuf, size_t textLen) {
  if (fromBuf && fromLen) {
    strncpy(fromBuf, g_line_from, fromLen - 1);
    fromBuf[fromLen - 1] = 0;
  }
  if (textBuf && textLen) {
    strncpy(textBuf, g_line_text, textLen - 1);
    textBuf[textLen - 1] = 0;
  }
}

int menu_list_last_scroll() {
  return s_menu_scroll >= 0 ? s_menu_scroll : 0;
}

}  // namespace display_nrf

#else

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace display_nrf {

namespace {

constexpr uint8_t kScreenW = 128;
constexpr uint8_t kScreenH = 64;
constexpr int8_t kOledRst = -1;

Adafruit_SSD1306 g_disp(kScreenW, kScreenH, &Wire, kOledRst);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

}  // namespace

bool init() {
  g_ok = false;
  Wire.setPins((uint8_t)PIN_I2C_SDA, (uint8_t)PIN_I2C_SCL);
  Wire.begin();
  Wire.setClock(100000);
  delay(50);
  if (!g_disp.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Wire.end();
    return false;
  }
  Wire.setClock(400000);
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  g_disp.display();
  g_ok = true;
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (line1) g_disp.println(line1);
  if (line2) g_disp.println(line2);
  g_disp.display();
}

void show_init_progress(int doneCount, int totalSteps, const char* statusLine) {
  if (!g_ok) return;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  g_disp.print(locale::getForDisplay("init_title"));
  const int cy = 22;
  const int r = 2;
  const int n = totalSteps;
  int pitch = n > 1 ? (kScreenW - 16) / (n - 1) : 0;
  if (pitch < 8) pitch = 8;
  if (pitch > 18) pitch = 18;
  const int startX = (int)kScreenW / 2 - ((n - 1) * pitch) / 2;
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitch;
    const int cx1 = startX + (i + 1) * pitch;
    g_disp.drawLine(cx0 + r, cy, cx1 - r, cy, SSD1306_WHITE);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitch;
    if (i < doneCount) {
      g_disp.fillCircle(cx, cy, r, SSD1306_WHITE);
    } else if (i == doneCount && doneCount < totalSteps) {
      g_disp.drawCircle(cx, cy, r, SSD1306_WHITE);
      g_disp.fillCircle(cx, cy, 1, SSD1306_WHITE);
    } else {
      g_disp.drawCircle(cx, cy, r, SSD1306_WHITE);
    }
  }
  if (statusLine && statusLine[0]) {
    g_disp.setCursor(0, 36);
    g_disp.print(statusLine);
  }
  g_disp.setCursor(0, 52);
  g_disp.print(locale::getForDisplay("init_hint"));
  g_disp.display();
}

void show_warning_blocking(const char* line1, const char* line2, uint32_t durationMs) {
  if (!g_ok) return;
  char body[220];
  snprintf(body, sizeof(body), "%s\n%s", line1 ? line1 : "", line2 ? line2 : "");
  show_fullscreen_text(locale::getForDisplay("warn_title"), body);
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < durationMs) {
    ble::update();
    delay(50);
  }
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.println(locale::getForDisplay("menu_selftest"));
  g_disp.printf("%s\n", radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  g_disp.printf("%s\n", antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  g_disp.printf("%s %umV\n", locale::getForDisplay("battery"), (unsigned)batteryMv);
  g_disp.printf("%s %u\n", locale::getForDisplay("selftest_heap"), (unsigned)heapFree);
  g_disp.display();
}

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  if (line1) g_disp.println(line1);
  if (line2) g_disp.println(line2);
  if (line3) g_disp.println(line3);
  if (line4) g_disp.println(line4);
  g_disp.display();
}

void queue_last_msg(const char* fromHex, const char* text) {
  if (!g_ok) return;
  g_line_from[0] = 0;
  g_line_text[0] = 0;
  if (fromHex) {
    strncpy(g_line_from, fromHex, sizeof(g_line_from) - 1);
    g_line_from[sizeof(g_line_from) - 1] = 0;
  }
  if (text) {
    strncpy(g_line_text, text, sizeof(g_line_text) - 1);
    g_line_text[sizeof(g_line_text) - 1] = 0;
  }
  g_last_dirty = true;
}

void poll() {
  if (!g_ok || !g_last_dirty) return;
  uint32_t now = millis();
  if ((uint32_t)(now - g_last_poll_ms) < 400) return;
  g_last_poll_ms = now;

  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.println(locale::getForDisplay("last_msg_title"));
  if (g_line_from[0]) {
    g_disp.println(g_line_from);
  }
  if (g_line_text[0]) {
    g_disp.println(g_line_text);
  }
  g_disp.display();
  g_last_dirty = false;
}

static constexpr int kMenuRowOled = 9;

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll, const char* footerHint,
    const uint8_t* const* icons) {
  (void)icons;
  if (!g_ok || !labels || count < 1) return;
  if (scroll < 0) scroll = 0;
  const int footerH = (footerHint && footerHint[0]) ? 8 : 0;
  const int maxRows = (kScreenH - 10 - footerH) / kMenuRowOled;
  if (scroll > selected) scroll = selected;
  if (scroll < selected - maxRows + 1) scroll = selected - maxRows + 1;
  if (scroll < 0) scroll = 0;

  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (title) g_disp.println(title);
  int y = 10;
  for (int row = 0; row < maxRows; row++) {
    int idx = scroll + row;
    if (idx >= count) break;
    const bool sel = (idx == selected);
    if (sel) g_disp.fillRect(0, y, kScreenW, kMenuRowOled, SSD1306_WHITE);
    g_disp.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    g_disp.setCursor(1, y + 1);
    if (labels[idx]) g_disp.print(labels[idx]);
    y += kMenuRowOled;
  }
  if (footerH > 0 && footerHint) {
    g_disp.setTextColor(SSD1306_WHITE);
    g_disp.setCursor(0, (int16_t)(kScreenH - 8));
    g_disp.print(footerHint);
  }
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.display();
}

void show_home_menu_strip(const char* title, const char* const* labels, const uint8_t* const* icons, int count,
    int selected, int scroll, const char* footerHint) {
  show_menu_list(title, labels, count, selected, scroll, footerHint, icons);
}

void show_fullscreen_text(const char* title, const char* body) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (title) g_disp.println(title);
  int y = 10;
  if (body && body[0]) {
    const char* p = body;
    char line[22];
    while (*p && y < 56) {
      size_t n = 0;
      while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
      line[n] = 0;
      if (*p == '\n') p++;
      g_disp.setCursor(0, y);
      g_disp.print(line);
      y += 8;
    }
  }
  g_disp.display();
}

void get_last_msg_peek(char* fromBuf, size_t fromLen, char* textBuf, size_t textLen) {
  if (fromBuf && fromLen) {
    strncpy(fromBuf, g_line_from, fromLen - 1);
    fromBuf[fromLen - 1] = 0;
  }
  if (textBuf && textLen) {
    strncpy(textBuf, g_line_text, textLen - 1);
    textBuf[textLen - 1] = 0;
  }
}

int menu_list_last_scroll() {
  return 0;
}

void apply_rotation_from_prefs() {}

}  // namespace display_nrf

#endif
