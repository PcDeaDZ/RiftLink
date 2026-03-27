/**
 * Абстракция ввода для UI: события навигации (кнопка / энкодер).
 */
#pragma once

#include <cstdint>

namespace ui_input {

enum class Event : uint8_t {
  None = 0,
  Up,       // предыдущий пункт / прокрутка вверх
  Down,     // следующий пункт / прокрутка вниз
  Select,   // подтверждение (часто long press на однокнопочных)
  Back,     // отмена / назад
  Secondary // доп. действие (например короткое на «вход» с HOME)
};

}  // namespace ui_input
