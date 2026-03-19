/**
 * Заглушки для libsodium AEGIS — отсутствуют в prebuilt Arduino 3.0 / IDF 5.1
 * (espressif/idf-extra-components#449, фикс в PR #450, но платформа 51.03.04 — до фикса).
 * Мы используем только ChaCha20-Poly1305 и crypto_box; AEGIS не нужен.
 * sodium_init() вызывает эти функции — достаточно вернуть 0.
 */
#ifdef __cplusplus
extern "C" {
#endif

int _crypto_aead_aegis128l_pick_best_implementation(void) {
  return 0;
}

int _crypto_aead_aegis256_pick_best_implementation(void) {
  return 0;
}

#ifdef __cplusplus
}
#endif
