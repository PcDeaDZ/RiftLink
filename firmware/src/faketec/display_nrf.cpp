/**
 * Дисплей nRF: Heltec T114 — встроенный ST7789 (SPI1); иначе SSD1306 I2C.
 */

#include "display_nrf.h"
#include "board_pins.h"
#include "locale/locale.h"

#include <Arduino.h>
#include <cstring>

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

namespace display_nrf {

namespace {

Adafruit_ST7789 g_tft(&SPI1, TFT_SPI_CS, TFT_SPI_DC, TFT_SPI_RST);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

/** Режим отрисовки — чтобы не делать fillScreen при каждом обновлении дашборда (как дельта в Meshtastic TFT). */
enum class ScreenKind : uint8_t { None, Boot, Status4, MenuFull, FullscreenText, Selftest, LastMsgPoll };
static ScreenKind g_screen_kind = ScreenKind::None;

static constexpr int16_t kTftW = 135;
static constexpr int16_t kTftH = 240;
/**
 * SPI после init: как в Meshtastic heltec_mesh_node_t114 variant.h — SPI_FREQUENCY 40 MHz (запись в ST7789).
 */
static constexpr uint32_t kT114TftSpiHz = 40000000u;
/** Полоса под 4 строки text size 2 (~16px/строка) с запасом на перенос длинных строк дашборда. */
static constexpr int16_t kStatus4ClearH = 160;

static constexpr int kMenuRowT114 = 12;
static constexpr int kMenuTitleH = 14;

/** Кэш списка меню — для перерисовки только двух строк при смене выделения (без чёрной вспышки на весь TFT). */
static const char* const* s_menu_labels_arr = nullptr;
static const char* s_menu_footer_ptr = nullptr;
static char s_menu_title_snap[40] = "";
static int s_menu_count = -1;
static int s_menu_scroll = -1;
static int s_menu_sel = -1;

/** Кэш 4 строк дашборда (text size 2): без повторной заливки при том же содержимом (ложные клики / двойные вызовы). */
static char s_status4_lines[4][48];
static bool s_status4_valid = false;

static void menu_paint_row_t114(int y, const char* label, bool selected) {
  if (selected) {
    g_tft.fillRect(0, y, kTftW, kMenuRowT114, ST77XX_WHITE);
    g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
  } else {
    g_tft.fillRect(0, y, kTftW, kMenuRowT114, ST77XX_BLACK);
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  }
  g_tft.setTextSize(1);
  g_tft.setCursor(2, y + 2);
  g_tft.setTextWrap(false);
  if (label) g_tft.print(label);
}

/**
 * Оценка высоты области дашборда (text size 2, ~12px ширина символа → ~11 символов в строке 135px).
 * Чем меньше чёрная полоса перед текстом при Status4→Status4, тем слабее «вспышка» на весь верх TFT.
 */
static int16_t status4_estimated_clear_height(const char lines[4][48]) {
  const int kCharsPerVisualRow = 11;
  int total = 0;
  for (int i = 0; i < 4; i++) {
    const size_t n = strlen(lines[i]);
    if (n == 0) continue;
    const int rows = (int)((n + (size_t)kCharsPerVisualRow - 1) / (size_t)kCharsPerVisualRow);
    total += rows * 16;
  }
  if (total < 64) total = 64;
  if (total > (int)kTftH) total = (int)kTftH;
  total += 8;
  if (total > (int)kTftH) total = (int)kTftH;
  return (int16_t)total;
}

/** Частичная заливка только при подряд двух Status4; иначе полный кадр (смена меню/формы). */
static void clear_for_screen(ScreenKind next, int16_t status4_dash_strip_h = -1) {
  if (next != ScreenKind::MenuFull) {
    s_menu_labels_arr = nullptr;
  }
  if (next != ScreenKind::Status4) {
    s_status4_valid = false;
  }
  const bool dash_refresh = (g_screen_kind == ScreenKind::Status4 && next == ScreenKind::Status4);
  if (dash_refresh) {
    int16_t h = kStatus4ClearH;
    if (status4_dash_strip_h > 0) {
      h = status4_dash_strip_h;
      if (h > kTftH) h = kTftH;
    }
    g_tft.fillRect(0, 0, kTftW, h, ST77XX_BLACK);
  } else {
    g_tft.fillRect(0, 0, kTftW, kTftH, ST77XX_BLACK);
  }
  g_screen_kind = next;
}

}  // namespace

bool init() {
  g_ok = false;
  // Heltec T114: VTFT_CTRL включает питание матрицы (Meshtastic: LOW = on). Иначе подсветка/шина могут «моргать», картинки нет.
  pinMode(TFT_VTFT_CTRL, OUTPUT);
  digitalWrite(TFT_VTFT_CTRL, TFT_VTFT_PWR_ON);
  delay(20);
  pinMode(TFT_BL, OUTPUT);
  // Подсветка после init (как в туториалах Adafruit): не показывать мусор на шине до первого чёрного кадра.
  digitalWrite(TFT_BL, TFT_BL_ON == LOW ? HIGH : LOW);
  delay(5);
  SPI1.setPins(TFT_SPI_MISO, TFT_SPI_SCK, TFT_SPI_MOSI);
  SPI1.begin();
  // 1.14" ST7789: Adafruit ожидает init(135, 240) для CASET/RASET и смещений 135×240 (см. Adafruit_ST7789.cpp).
  g_tft.init(135, 240);
  g_tft.setRotation(0);
  g_tft.setSPISpeed(kT114TftSpiHz);
  // Белый на чёрном — перерисовка символов без «просветов»; ниже кадры батчим startWrite/endWrite (меньше артефактов на ST7789).
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.setTextWrap(false);
  g_tft.startWrite();
  g_tft.fillRect(0, 0, kTftW, kTftH, ST77XX_BLACK);
  g_tft.endWrite();
  digitalWrite(TFT_BL, TFT_BL_ON);
  g_tft.setCursor(0, 0);
  g_screen_kind = ScreenKind::None;
  g_ok = true;
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Boot);
  g_tft.setCursor(0, 0);
  if (line1) g_tft.println(line1);
  if (line2) g_tft.println(line2);
  g_tft.endWrite();
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Selftest);
  g_tft.setCursor(0, 0);
  g_tft.println(locale::getForDisplay("menu_selftest"));
  g_tft.printf("%s\n", radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  g_tft.printf("%s\n", antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  g_tft.printf("%s %umV\n", locale::getForDisplay("battery"), (unsigned)batteryMv);
  g_tft.printf("%s %u\n", locale::getForDisplay("selftest_heap"), (unsigned)heapFree);
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

  int16_t dash_strip_h = -1;
  if (g_screen_kind == ScreenKind::Status4) {
    dash_strip_h = status4_estimated_clear_height(next4);
  }

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Status4, dash_strip_h);
  g_tft.setCursor(0, 0);
  if (line1) g_tft.println(line1);
  if (line2) g_tft.println(line2);
  if (line3) g_tft.println(line3);
  if (line4) g_tft.println(line4);
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
  g_tft.setTextSize(2);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::LastMsgPoll);
  g_tft.setCursor(0, 0);
  g_tft.println(locale::getForDisplay("last_msg_title"));
  if (g_line_from[0]) g_tft.println(g_line_from);
  if (g_line_text[0]) g_tft.println(g_line_text);
  g_tft.endWrite();
  g_last_dirty = false;
}

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll, const char* footerHint) {
  if (!g_ok || !labels || count < 1) return;
  if (scroll < 0) scroll = 0;
  if (scroll > count - 1) scroll = count - 1;
  const int footerH = (footerHint && footerHint[0]) ? 12 : 0;
  const int maxRows = (240 - kMenuTitleH - footerH) / kMenuRowT114;
  if (scroll > selected) scroll = selected;
  if (scroll < selected - maxRows + 1) scroll = selected - maxRows + 1;
  if (scroll < 0) scroll = 0;

  bool title_same = false;
  if (!title || !title[0]) {
    title_same = (s_menu_title_snap[0] == 0);
  } else {
    title_same = (strcmp(s_menu_title_snap, title) == 0);
  }
  const bool same_list = (labels == s_menu_labels_arr && count == s_menu_count && scroll == s_menu_scroll &&
                          footerHint == s_menu_footer_ptr && title_same);
  if (same_list && selected == s_menu_sel && s_menu_sel >= 0) {
    return;
  }
  if (same_list && selected != s_menu_sel && s_menu_sel >= 0) {
    const int old_row_index = s_menu_sel - scroll;
    const int new_row_index = selected - scroll;
    if (old_row_index >= 0 && old_row_index < maxRows && new_row_index >= 0 && new_row_index < maxRows &&
        s_menu_sel < count && selected < count) {
      const int y_old = kMenuTitleH + old_row_index * kMenuRowT114;
      const int y_new = kMenuTitleH + new_row_index * kMenuRowT114;
      g_tft.setTextWrap(false);
      g_tft.startWrite();
      menu_paint_row_t114(y_old, labels[s_menu_sel], false);
      menu_paint_row_t114(y_new, labels[selected], true);
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      g_tft.endWrite();
      s_menu_sel = selected;
      return;
    }
  }

  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::MenuFull);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, 2);
  if (title) g_tft.print(title);
  g_tft.drawFastHLine(0, kMenuTitleH - 1, 135, ST77XX_WHITE);

  int y = kMenuTitleH;
  for (int row = 0; row < maxRows; row++) {
    int idx = scroll + row;
    if (idx >= count) break;
    const bool sel = (idx == selected);
    if (sel) {
      g_tft.fillRect(0, y, 135, kMenuRowT114, ST77XX_WHITE);
      g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    } else {
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
    g_tft.setCursor(2, y + 2);
    if (labels[idx]) g_tft.print(labels[idx]);
    y += kMenuRowT114;
  }
  if (footerH > 0 && footerHint) {
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    g_tft.setCursor(0, 228);
    g_tft.print(footerHint);
  }
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.endWrite();

  s_menu_labels_arr = labels;
  s_menu_footer_ptr = footerHint;
  if (title) {
    strncpy(s_menu_title_snap, title, sizeof(s_menu_title_snap) - 1);
    s_menu_title_snap[sizeof(s_menu_title_snap) - 1] = 0;
  } else {
    s_menu_title_snap[0] = 0;
  }
  s_menu_count = count;
  s_menu_scroll = scroll;
  s_menu_sel = selected;
}

void show_fullscreen_text(const char* title, const char* body) {
  if (!g_ok) return;
  g_tft.startWrite();
  clear_for_screen(ScreenKind::FullscreenText);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, 2);
  if (title) g_tft.println(title);
  g_tft.drawFastHLine(0, 12, 135, ST77XX_WHITE);
  int y = 16;
  if (body && body[0]) {
    const char* p = body;
    char line[28];
    while (*p && y < 228) {
      size_t n = 0;
      while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
      line[n] = 0;
      if (*p == '\n') p++;
      g_tft.setCursor(0, y);
      g_tft.print(line);
      y += 10;
    }
  }
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

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll, const char* footerHint) {
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

}  // namespace display_nrf

#endif
