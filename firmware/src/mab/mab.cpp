/**
 * MAB — ε-greedy Multi-Armed Bandit для задержки retry
 */

#include "mab.h"
#include <math.h>
#include <string.h>
#if defined(RIFTLINK_NRF52)
#include <Arduino.h>
static inline uint32_t mab_rand32() {
  return (uint32_t)random(0x7fffffff);
}
#else
#include <esp_random.h>
static inline uint32_t mab_rand32() {
  return esp_random();
}
#endif

namespace mab {

static float s_sum[NUM_ARMS];
static uint32_t s_count[NUM_ARMS];
static uint32_t s_totalPulls = 0;
static constexpr float EPSILON = 0.2f;

// Диапазоны задержки в ms: fast 50–150, short 200–350, medium 400–600, long 700–1000
static const uint32_t s_delayMin[NUM_ARMS] = {50, 200, 400, 700};
static const uint32_t s_delayMax[NUM_ARMS] = {150, 350, 600, 1000};

void init() {
  memset(s_sum, 0, sizeof(s_sum));
  memset(s_count, 0, sizeof(s_count));
  s_totalPulls = 0;
}

int selectAction() {
  for (int i = 0; i < NUM_ARMS; i++) {
    if (s_count[i] == 0) return i;  // explore неиспробованные
  }
  if ((mab_rand32() % 100) < (int)(EPSILON * 100)) {
    return mab_rand32() % NUM_ARMS;  // ε-greedy: случайное
  }
  float bestMean = -1e9f;
  int best = 0;
  for (int i = 0; i < NUM_ARMS; i++) {
    float mean = s_sum[i] / (float)s_count[i];
    if (mean > bestMean) {
      bestMean = mean;
      best = i;
    }
  }
  return best;
}

uint32_t getDelayMs(int action) {
  if (action < 0 || action >= NUM_ARMS) action = 1;
  uint32_t range = s_delayMax[action] - s_delayMin[action];
  return s_delayMin[action] + (mab_rand32() % (range + 1));
}

void reward(int action, int rewardVal) {
  if (action < 0 || action >= NUM_ARMS) return;
  s_sum[action] += (float)rewardVal;
  s_count[action]++;
  s_totalPulls++;
}

}  // namespace mab
