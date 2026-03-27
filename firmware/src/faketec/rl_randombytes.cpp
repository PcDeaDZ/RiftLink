/**
 * randombytes для libsodium на nRF52840 — аппаратный RNG (NRF_RNG).
 * Устанавливается до sodium_init(); иначе sysrandom может не работать без /dev/urandom.
 */

#include "rl_randombytes.h"
#include <Arduino.h>
#include <string.h>

extern "C" {
#include "randombytes.h"
}

#if defined(NRF52840_XXAA)
#include <nrf.h>
#endif

static const char* impl_name(void) {
  return "nrf52840_rng";
}

static void impl_buf(void* const buf, const size_t size) {
  uint8_t* p = (uint8_t*)buf;
#if defined(NRF52840_XXAA)
  NRF_RNG->CONFIG = 0;
  NRF_RNG->EVENTS_VALRDY = 0;
  NRF_RNG->TASKS_START = 1;
  for (size_t i = 0; i < size; i++) {
    while (!NRF_RNG->EVENTS_VALRDY) {
      __NOP();
    }
    NRF_RNG->EVENTS_VALRDY = 0;
    p[i] = (uint8_t)NRF_RNG->VALUE;
  }
  NRF_RNG->TASKS_STOP = 1;
#else
  for (size_t i = 0; i < size; i++) {
    p[i] = (uint8_t)random(256);
  }
#endif
}

static uint32_t impl_random(void) {
  uint32_t v;
  impl_buf(&v, sizeof v);
  return v;
}

static void impl_stir(void) {}

static const randombytes_implementation nrf_rng_impl = {
    impl_name,
    impl_random,
    impl_stir,
    nullptr,
    impl_buf,
    nullptr,
};

void rl_randombytes_install() {
  randombytes_set_implementation(&nrf_rng_impl);
}
