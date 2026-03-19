/**
 * MAB — ε-greedy Multi-Armed Bandit для задержки retry
 */

#include "mab.h"
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace mab {

static float s_sum[NUM_ARMS];
static uint32_t s_count[NUM_ARMS];
static uint32_t s_totalPulls = 0;
static constexpr float EPSILON = 0.2f;

// Диапазоны задержки в ms: short 200–350, medium 400–600, long 700–1000
static const uint32_t s_delayMin[NUM_ARMS] = {200, 400, 700};
static const uint32_t s_delayMax[NUM_ARMS] = {350, 600, 1000};

void init() {
  memset(s_sum, 0, sizeof(s_sum));
  memset(s_count, 0, sizeof(s_count));
  s_totalPulls = 0;
}

int selectAction() {
  for (int i = 0; i < NUM_ARMS; i++) {
    if (s_count[i] == 0) return i;  // explore неиспробованные
  }
  if ((esp_random() % 100) < (int)(EPSILON * 100)) {
    return esp_random() % NUM_ARMS;  // ε-greedy: случайное
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
  return s_delayMin[action] + (esp_random() % (range + 1));
}

void reward(int action, int rewardVal) {
  if (action < 0 || action >= NUM_ARMS) return;
  s_sum[action] += (float)rewardVal;
  s_count[action]++;
  s_totalPulls++;
}

}  // namespace mab
