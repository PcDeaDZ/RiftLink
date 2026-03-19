/**
 * MAB — Multi-Armed Bandit для выбора задержки retry
 * Действия: delay_short, delay_medium, delay_long
 * Награда: +1 при ACK, −1 при NACK/undelivered
 */

#pragma once

#include <cstdint>

namespace mab {

constexpr int NUM_ARMS = 3;

void init();

/** Выбрать действие (0..2). Возвращает индекс руки. */
int selectAction();

/** Получить задержку в ms для действия (jitter внутри диапазона). */
uint32_t getDelayMs(int action);

/** Сообщить награду за действие. reward: +1 ACK, −1 NACK/undelivered */
void reward(int action, int reward);

}  // namespace mab
