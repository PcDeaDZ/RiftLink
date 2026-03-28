#pragma once

#include <cstddef>
#include <cstdint>

namespace riftlink_kv {

bool begin();
/** true после успешного begin() (InternalFS смонтирован). */
bool is_ready();
bool getBlob(const char* key, uint8_t* buf, size_t* len);
bool setBlob(const char* key, const uint8_t* buf, size_t len);
bool getU32(const char* key, uint32_t* out);
bool setU32(const char* key, uint32_t v);
bool getI8(const char* key, int8_t* out);
bool setI8(const char* key, int8_t v);

}  // namespace riftlink_kv
