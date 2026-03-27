#pragma once

/** Установить реализацию randombytes для libsodium (аппаратный RNG nRF52) до sodium_init(). */
void rl_randombytes_install();
