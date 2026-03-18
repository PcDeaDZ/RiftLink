/**
 * RiftLink — сжатие LZ4 для текста
 * Порядок: Сжатие → Шифрование (план §4.4)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace compress {

// Минимальная длина для сжатия (короткие сообщения не выигрывают)
constexpr size_t MIN_LEN_TO_COMPRESS = 50;

// Сжать plain в out. Формат: [orig_size 2B LE][lz4_data]
// Возвращает размер в out или 0 если сжатие не выгодно/ошибка
size_t compress(const uint8_t* plain, size_t plainLen, uint8_t* out, size_t outMaxLen);

// Распаковать. compData = [orig_size 2B][lz4...], compLen — полный размер
// out — буфер для распакованных данных, outMaxLen — его размер
// Возвращает размер распакованных данных или 0 при ошибке
size_t decompress(const uint8_t* compData, size_t compLen, uint8_t* out, size_t outMaxLen);

}  // namespace compress
