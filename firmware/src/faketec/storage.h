/**
 * FakeTech Storage — замена NVS (nRF52 Flash)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace storage {

bool init();

bool getBlob(const char* key, uint8_t* out, size_t* len);
bool setBlob(const char* key, const uint8_t* data, size_t len);

bool getStr(const char* key, char* out, size_t maxLen);
bool setStr(const char* key, const char* value);

bool getU32(const char* key, uint32_t* out);
bool setU32(const char* key, uint32_t value);

bool getI8(const char* key, int8_t* out);
bool setI8(const char* key, int8_t value);

}  // namespace storage
