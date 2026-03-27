/**
 * Тесты логики UI без железа: прокрутка списков, RSSI→полоски, смещение текста сообщения.
 * Запуск: cd firmware && pio test -e native
 */

#include <cstring>
#include <unity.h>
#include "ui/ui_msg_scroll.h"
#include "ui/ui_scroll.h"
#include "ui/ui_topbar_model.h"

/* setUp/tearDown — в test_packet.cpp (единый exe). */

void test_sync_list_window_aligns_to_selection() {
  int scrollOff = 0;
  ui_scroll::syncListWindow(5, 10, 3, scrollOff);
  TEST_ASSERT_EQUAL(3, scrollOff);

  scrollOff = 0;
  ui_scroll::syncListWindow(0, 10, 3, scrollOff);
  TEST_ASSERT_EQUAL(0, scrollOff);

  scrollOff = 5;
  ui_scroll::syncListWindow(2, 10, 3, scrollOff);
  TEST_ASSERT_EQUAL(2, scrollOff);
}

void test_sync_list_window_single_row() {
  int scrollOff = 0;
  ui_scroll::syncListWindow(4, 5, 1, scrollOff);
  TEST_ASSERT_EQUAL(4, scrollOff);
}

void test_rssi_to_bars() {
  TEST_ASSERT_EQUAL(4, ui_topbar::rssiToBars(-70));
  TEST_ASSERT_EQUAL(3, ui_topbar::rssiToBars(-80));
  TEST_ASSERT_EQUAL(2, ui_topbar::rssiToBars(-90));
  TEST_ASSERT_EQUAL(1, ui_topbar::rssiToBars(-100));
  TEST_ASSERT_EQUAL(0, ui_topbar::rssiToBars(-110));
}

void test_msg_scroll_advance_wraps() {
  const char t[] = "abcd";
  size_t scroll = 0;
  ui_msg_scroll::advanceOneLine(scroll, t, strlen(t), 4);
  TEST_ASSERT_EQUAL(0, scroll);
}

void test_msg_scroll_advance_two_lines() {
  const char t[] = "abcdefgh";
  size_t scroll = 0;
  ui_msg_scroll::advanceOneLine(scroll, t, strlen(t), 4);
  TEST_ASSERT_EQUAL(4, scroll);
  ui_msg_scroll::advanceOneLine(scroll, t, strlen(t), 4);
  TEST_ASSERT_EQUAL(0, scroll);
}

void test_msg_scroll_overflow_hint() {
  const char t[] = "aaaaaaaaaaaaaaaa";  // 16 chars
  TEST_ASSERT_TRUE(ui_msg_scroll::hasOverflowPastLines(t, strlen(t), 0, 4, 2));
  TEST_ASSERT_FALSE(ui_msg_scroll::hasOverflowPastLines(t, strlen(t), 0, 4, 8));
}

/* main() — в test_packet.cpp: все native-тесты линкуются в один exe, дублировать main нельзя. */
