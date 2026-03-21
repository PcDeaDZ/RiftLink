/**
 * RiftLink Groups — подписка на группы
 */

#include "groups.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_GROUPS "grps"
#define NVS_KEY_GROUP_KEYS "grpk"
#define NVS_KEY_GROUP_KEY_MASK "grpm"
#define NVS_KEY_GROUP_KEY_VER "grpv"

static uint32_t s_groups[MAX_GROUPS];
static uint8_t s_groupKeys[MAX_GROUPS][32];
static uint16_t s_groupKeyVersion[MAX_GROUPS];
static uint8_t s_groupKeyMask = 0;
static int s_count = 0;
static bool s_inited = false;

namespace groups {

static int findIndex(uint32_t groupId) {
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == groupId) return i;
  }
  return -1;
}

static bool hasKeyAt(int index) {
  if (index < 0 || index >= MAX_GROUPS) return false;
  return (s_groupKeyMask & (1u << index)) != 0;
}

static void setKeyAt(int index, const uint8_t* key32) {
  if (index < 0 || index >= MAX_GROUPS || !key32) return;
  memcpy(s_groupKeys[index], key32, 32);
  s_groupKeyMask = (uint8_t)(s_groupKeyMask | (1u << index));
}

static void clearKeyAt(int index) {
  if (index < 0 || index >= MAX_GROUPS) return;
  memset(s_groupKeys[index], 0, 32);
  s_groupKeyMask = (uint8_t)(s_groupKeyMask & (uint8_t)~(1u << index));
  s_groupKeyVersion[index] = 0;
}

static void persist() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  if (s_count > 0) nvs_set_blob(h, NVS_KEY_GROUPS, s_groups, s_count * sizeof(uint32_t));
  else nvs_erase_key(h, NVS_KEY_GROUPS);
  nvs_set_blob(h, NVS_KEY_GROUP_KEYS, s_groupKeys, sizeof(s_groupKeys));
  nvs_set_blob(h, NVS_KEY_GROUP_KEY_VER, s_groupKeyVersion, sizeof(s_groupKeyVersion));
  nvs_set_u8(h, NVS_KEY_GROUP_KEY_MASK, s_groupKeyMask);
  nvs_commit(h);
  nvs_close(h);
}

void init() {
  if (s_inited) return;
  memset(s_groups, 0, sizeof(s_groups));
  memset(s_groupKeys, 0, sizeof(s_groupKeys));
  memset(s_groupKeyVersion, 0, sizeof(s_groupKeyVersion));
  s_groupKeyMask = 0;
  s_count = 0;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(s_groups);
    if (nvs_get_blob(h, NVS_KEY_GROUPS, s_groups, &len) == ESP_OK) {
      s_count = len / sizeof(uint32_t);
      if (s_count > MAX_GROUPS) s_count = MAX_GROUPS;
    }
    size_t keysLen = sizeof(s_groupKeys);
    if (nvs_get_blob(h, NVS_KEY_GROUP_KEYS, s_groupKeys, &keysLen) != ESP_OK || keysLen != sizeof(s_groupKeys)) {
      memset(s_groupKeys, 0, sizeof(s_groupKeys));
    }
    size_t verLen = sizeof(s_groupKeyVersion);
    if (nvs_get_blob(h, NVS_KEY_GROUP_KEY_VER, s_groupKeyVersion, &verLen) != ESP_OK || verLen != sizeof(s_groupKeyVersion)) {
      memset(s_groupKeyVersion, 0, sizeof(s_groupKeyVersion));
    }
    uint8_t mask = 0;
    if (nvs_get_u8(h, NVS_KEY_GROUP_KEY_MASK, &mask) == ESP_OK) s_groupKeyMask = mask;
    nvs_close(h);
  }
  // Раньше при пустом NVS подмешивали GROUP_ALL (1) — в UI это выглядело как «лишняя» группа.
  // Приём broadcast (groupId==GROUP_ALL) не требует записи в NVS (см. main.cpp OP_GROUP_MSG).
  uint32_t newGroups[MAX_GROUPS] = {0};
  uint8_t newKeys[MAX_GROUPS][32] = {{0}};
  uint16_t newVer[MAX_GROUPS] = {0};
  uint8_t newMask = 0;
  int newCount = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == GROUP_ALL) continue;
    if (newCount >= MAX_GROUPS) break;
    newGroups[newCount] = s_groups[i];
    memcpy(newKeys[newCount], s_groupKeys[i], 32);
    newVer[newCount] = s_groupKeyVersion[i];
    if ((s_groupKeyMask & (1u << i)) != 0) newMask = (uint8_t)(newMask | (1u << newCount));
    newCount++;
  }
  if (newCount != s_count) {
    memcpy(s_groups, newGroups, sizeof(s_groups));
    memcpy(s_groupKeys, newKeys, sizeof(s_groupKeys));
    memcpy(s_groupKeyVersion, newVer, sizeof(s_groupKeyVersion));
    s_groupKeyMask = newMask;
    s_count = newCount;
    persist();
  }
  s_inited = true;
}

bool isInGroup(uint32_t groupId) {
  if (!s_inited) return false;
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == groupId) return true;
  }
  return false;
}

bool addGroup(uint32_t groupId) {
  if (groupId == GROUP_ALL) return false;  // зарезервировано под mesh broadcast
  if (isInGroup(groupId)) return true;
  if (s_count >= MAX_GROUPS) return false;

  s_groups[s_count++] = groupId;
  clearKeyAt(s_count - 1);
  persist();
  return true;
}

int getCount() {
  return s_count;
}

uint32_t getId(int index) {
  if (index < 0 || index >= s_count) return 0;
  return s_groups[index];
}

void removeGroup(uint32_t groupId) {
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == groupId) {
      const uint8_t oldMask = s_groupKeyMask;
      for (int j = i; j < s_count - 1; j++) s_groups[j] = s_groups[j + 1];
      for (int j = i; j < s_count - 1; j++) memcpy(s_groupKeys[j], s_groupKeys[j + 1], 32);
      for (int j = i; j < s_count - 1; j++) s_groupKeyVersion[j] = s_groupKeyVersion[j + 1];
      s_count--;
      // Пересобрать битовую маску после сдвига.
      uint8_t newMask = 0;
      for (int j = 0; j < s_count; j++) {
        int oldIdx = (j < i) ? j : (j + 1);
        if ((oldMask & (1u << oldIdx)) != 0) newMask = (uint8_t)(newMask | (1u << j));
      }
      s_groupKeyMask = newMask;
      clearKeyAt(s_count);
      persist();
      return;
    }
  }
}

bool setGroupKey(uint32_t groupId, const uint8_t* key32, uint16_t keyVersion) {
  if (!key32) return false;
  int idx = findIndex(groupId);
  if (idx < 0) {
    if (!addGroup(groupId)) return false;
    idx = findIndex(groupId);
    if (idx < 0) return false;
  }
  setKeyAt(idx, key32);
  if (keyVersion == 0) {
    uint16_t prev = s_groupKeyVersion[idx];
    s_groupKeyVersion[idx] = (uint16_t)(prev == 0xFFFF ? 1 : (prev + 1));
    if (s_groupKeyVersion[idx] == 0) s_groupKeyVersion[idx] = 1;
  } else {
    s_groupKeyVersion[idx] = keyVersion;
  }
  persist();
  return true;
}

bool clearGroupKey(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  clearKeyAt(idx);
  persist();
  return true;
}

bool hasGroupKey(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  return hasKeyAt(idx);
}

bool getGroupKey(uint32_t groupId, uint8_t* out32) {
  if (!out32) return false;
  int idx = findIndex(groupId);
  if (idx < 0 || !hasKeyAt(idx)) return false;
  memcpy(out32, s_groupKeys[idx], 32);
  return true;
}

uint16_t getGroupKeyVersion(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0 || !hasKeyAt(idx)) return 0;
  return s_groupKeyVersion[idx];
}

bool isPrivateAt(int index) {
  return hasKeyAt(index);
}

uint16_t keyVersionAt(int index) {
  if (index < 0 || index >= s_count || !hasKeyAt(index)) return 0;
  return s_groupKeyVersion[index];
}

}  // namespace groups
