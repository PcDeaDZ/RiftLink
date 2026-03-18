/**
 * RiftLink — LZ4 сжатие
 */

#include "compress.h"
#include <lz4.h>
#include <string.h>

namespace compress {

size_t compress(const uint8_t* plain, size_t plainLen, uint8_t* out, size_t outMaxLen) {
  if (plainLen < MIN_LEN_TO_COMPRESS || plainLen > 4096) return 0;

  int maxComp = LZ4_compressBound((int)plainLen);
  if (outMaxLen < (size_t)(2 + maxComp)) return 0;

  int compLen = LZ4_compress_default(
      (const char*)plain, (char*)(out + 2), (int)plainLen, maxComp);
  if (compLen <= 0) return 0;

  // Сжатие выгодно только если результат меньше оригинала
  if ((size_t)(2 + compLen) >= plainLen) return 0;

  out[0] = (uint8_t)(plainLen & 0xFF);
  out[1] = (uint8_t)(plainLen >> 8);
  return 2 + (size_t)compLen;
}

size_t decompress(const uint8_t* compData, size_t compLen, uint8_t* out, size_t outMaxLen) {
  if (compLen < 3) return 0;  // минимум 2 байта размера + 1 байт данных

  uint16_t origSize = (uint16_t)compData[0] | ((uint16_t)compData[1] << 8);
  if (origSize == 0 || origSize > outMaxLen) return 0;

  const char* src = (const char*)(compData + 2);
  int srcLen = (int)(compLen - 2);
  int decLen = LZ4_decompress_safe(src, (char*)out, srcLen, (int)origSize);
  if (decLen < 0) return 0;
  return (size_t)decLen;
}

}  // namespace compress
