/**
 * RiftLink Groups — подписка на группы
 */

#include "groups.h"
#include "node/node.h"
#include "protocol/packet.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_GROUPS "grps"
#define NVS_KEY_GROUP_KEYS "grpk"
#define NVS_KEY_GROUP_KEY_MASK "grpm"
#define NVS_KEY_GROUP_KEY_VER "grpv"
#define NVS_KEY_GROUP_OWNERS "grpo"
#define NVS_KEY_GROUP_OWNER_MASK "grpom"
#define NVS_KEY_GROUP_CAPS "grpc"
#define NVS_KEY_GROUP_CAP_MASK "grpcm"

static uint32_t s_groups[MAX_GROUPS];
static uint8_t s_groupKeys[MAX_GROUPS][32];
static uint16_t s_groupKeyVersion[MAX_GROUPS];
static uint8_t s_groupOwners[MAX_GROUPS][protocol::NODE_ID_LEN];
static uint8_t s_groupAdminCaps[MAX_GROUPS][8];
static uint8_t s_groupKeyMask = 0;
static uint8_t s_groupOwnerMask = 0;
static uint8_t s_groupAdminCapMask = 0;
static int s_count = 0;
static bool s_inited = false;

namespace groups {

static int findIndex(uint32_t groupId) {
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == groupId) return i;
  }
  return -1;
}

static bool hasPrivateKeyAt(int index) {
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

static bool hasOwnerAtInternal(int index) {
  if (index < 0 || index >= MAX_GROUPS) return false;
  return (s_groupOwnerMask & (1u << index)) != 0;
}

static void setOwnerAt(int index, const uint8_t* owner8) {
  if (index < 0 || index >= MAX_GROUPS || !owner8) return;
  memcpy(s_groupOwners[index], owner8, protocol::NODE_ID_LEN);
  s_groupOwnerMask = (uint8_t)(s_groupOwnerMask | (1u << index));
}

static void clearOwnerAt(int index) {
  if (index < 0 || index >= MAX_GROUPS) return;
  memset(s_groupOwners[index], 0, sizeof(s_groupOwners[index]));
  s_groupOwnerMask = (uint8_t)(s_groupOwnerMask & (uint8_t)~(1u << index));
}

static bool hasAdminCapAtInternal(int index) {
  if (index < 0 || index >= MAX_GROUPS) return false;
  return (s_groupAdminCapMask & (1u << index)) != 0;
}

static void setAdminCapAt(int index, const uint8_t* cap8) {
  if (index < 0 || index >= MAX_GROUPS || !cap8) return;
  memcpy(s_groupAdminCaps[index], cap8, 8);
  s_groupAdminCapMask = (uint8_t)(s_groupAdminCapMask | (1u << index));
}

static void clearAdminCapAt(int index) {
  if (index < 0 || index >= MAX_GROUPS) return;
  memset(s_groupAdminCaps[index], 0, sizeof(s_groupAdminCaps[index]));
  s_groupAdminCapMask = (uint8_t)(s_groupAdminCapMask & (uint8_t)~(1u << index));
}

static void persist() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  if (s_count > 0) nvs_set_blob(h, NVS_KEY_GROUPS, s_groups, s_count * sizeof(uint32_t));
  else nvs_erase_key(h, NVS_KEY_GROUPS);
  nvs_set_blob(h, NVS_KEY_GROUP_KEYS, s_groupKeys, sizeof(s_groupKeys));
  nvs_set_blob(h, NVS_KEY_GROUP_KEY_VER, s_groupKeyVersion, sizeof(s_groupKeyVersion));
  nvs_set_u8(h, NVS_KEY_GROUP_KEY_MASK, s_groupKeyMask);
  nvs_set_blob(h, NVS_KEY_GROUP_OWNERS, s_groupOwners, sizeof(s_groupOwners));
  nvs_set_u8(h, NVS_KEY_GROUP_OWNER_MASK, s_groupOwnerMask);
  nvs_set_blob(h, NVS_KEY_GROUP_CAPS, s_groupAdminCaps, sizeof(s_groupAdminCaps));
  nvs_set_u8(h, NVS_KEY_GROUP_CAP_MASK, s_groupAdminCapMask);
  nvs_commit(h);
  nvs_close(h);
}

void init() {
  if (s_inited) return;
  memset(s_groups, 0, sizeof(s_groups));
  memset(s_groupKeys, 0, sizeof(s_groupKeys));
  memset(s_groupKeyVersion, 0, sizeof(s_groupKeyVersion));
  memset(s_groupOwners, 0, sizeof(s_groupOwners));
  memset(s_groupAdminCaps, 0, sizeof(s_groupAdminCaps));
  s_groupKeyMask = 0;
  s_groupOwnerMask = 0;
  s_groupAdminCapMask = 0;
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
    size_t ownerLen = sizeof(s_groupOwners);
    if (nvs_get_blob(h, NVS_KEY_GROUP_OWNERS, s_groupOwners, &ownerLen) != ESP_OK || ownerLen != sizeof(s_groupOwners)) {
      memset(s_groupOwners, 0, sizeof(s_groupOwners));
    }
    uint8_t ownerMask = 0;
    if (nvs_get_u8(h, NVS_KEY_GROUP_OWNER_MASK, &ownerMask) == ESP_OK) s_groupOwnerMask = ownerMask;
    size_t capLen = sizeof(s_groupAdminCaps);
    if (nvs_get_blob(h, NVS_KEY_GROUP_CAPS, s_groupAdminCaps, &capLen) != ESP_OK || capLen != sizeof(s_groupAdminCaps)) {
      memset(s_groupAdminCaps, 0, sizeof(s_groupAdminCaps));
    }
    uint8_t capMask = 0;
    if (nvs_get_u8(h, NVS_KEY_GROUP_CAP_MASK, &capMask) == ESP_OK) s_groupAdminCapMask = capMask;
    nvs_close(h);
  }
  // Раньше при пустом NVS подмешивали GROUP_ALL (1) — в UI это выглядело как «лишняя» группа.
  // Приём broadcast (groupId==GROUP_ALL) не требует записи в NVS (см. main.cpp OP_GROUP_MSG).
  uint32_t newGroups[MAX_GROUPS] = {0};
  uint8_t newKeys[MAX_GROUPS][32] = {{0}};
  uint16_t newVer[MAX_GROUPS] = {0};
  uint8_t newMask = 0;
  uint8_t newOwners[MAX_GROUPS][protocol::NODE_ID_LEN] = {{0}};
  uint8_t newOwnerMask = 0;
  uint8_t newCaps[MAX_GROUPS][8] = {{0}};
  uint8_t newCapMask = 0;
  int newCount = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_groups[i] == GROUP_ALL) continue;
    if (newCount >= MAX_GROUPS) break;
    newGroups[newCount] = s_groups[i];
    memcpy(newKeys[newCount], s_groupKeys[i], 32);
    newVer[newCount] = s_groupKeyVersion[i];
    if ((s_groupKeyMask & (1u << i)) != 0) newMask = (uint8_t)(newMask | (1u << newCount));
    memcpy(newOwners[newCount], s_groupOwners[i], protocol::NODE_ID_LEN);
    if ((s_groupOwnerMask & (1u << i)) != 0) newOwnerMask = (uint8_t)(newOwnerMask | (1u << newCount));
    memcpy(newCaps[newCount], s_groupAdminCaps[i], 8);
    if ((s_groupAdminCapMask & (1u << i)) != 0) newCapMask = (uint8_t)(newCapMask | (1u << newCount));
    newCount++;
  }
  if (newCount != s_count) {
    memcpy(s_groups, newGroups, sizeof(s_groups));
    memcpy(s_groupKeys, newKeys, sizeof(s_groupKeys));
    memcpy(s_groupKeyVersion, newVer, sizeof(s_groupKeyVersion));
    s_groupKeyMask = newMask;
    memcpy(s_groupOwners, newOwners, sizeof(s_groupOwners));
    s_groupOwnerMask = newOwnerMask;
    memcpy(s_groupAdminCaps, newCaps, sizeof(s_groupAdminCaps));
    s_groupAdminCapMask = newCapMask;
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
      for (int j = i; j < s_count - 1; j++) memcpy(s_groupOwners[j], s_groupOwners[j + 1], protocol::NODE_ID_LEN);
      for (int j = i; j < s_count - 1; j++) memcpy(s_groupAdminCaps[j], s_groupAdminCaps[j + 1], 8);
      s_count--;
      // Пересобрать битовую маску после сдвига.
      uint8_t newMask = 0;
      uint8_t newOwnerMask = 0;
      uint8_t newCapMask = 0;
      for (int j = 0; j < s_count; j++) {
        int oldIdx = (j < i) ? j : (j + 1);
        if ((oldMask & (1u << oldIdx)) != 0) newMask = (uint8_t)(newMask | (1u << j));
        if ((s_groupOwnerMask & (1u << oldIdx)) != 0) newOwnerMask = (uint8_t)(newOwnerMask | (1u << j));
        if ((s_groupAdminCapMask & (1u << oldIdx)) != 0) newCapMask = (uint8_t)(newCapMask | (1u << j));
      }
      s_groupKeyMask = newMask;
      s_groupOwnerMask = newOwnerMask;
      s_groupAdminCapMask = newCapMask;
      clearKeyAt(s_count);
      clearOwnerAt(s_count);
      clearAdminCapAt(s_count);
      persist();
      return;
    }
  }
}

bool canRotateGroupKey(uint32_t groupId, const uint8_t* requesterId, const uint8_t* presentedAdminCapability8) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  if (!hasPrivateKeyAt(idx)) return true;
  // Legacy migration path: private key existed before owner metadata.
  if (!hasOwnerAtInternal(idx)) return requesterId != nullptr;
  if (requesterId && hasOwnerAtInternal(idx) &&
      memcmp(s_groupOwners[idx], requesterId, protocol::NODE_ID_LEN) == 0) {
    return true;
  }
  if (presentedAdminCapability8 && hasAdminCapAtInternal(idx) &&
      memcmp(s_groupAdminCaps[idx], presentedAdminCapability8, 8) == 0) {
    return true;
  }
  return false;
}

bool setGroupKey(uint32_t groupId, const uint8_t* key32, uint16_t keyVersion, const uint8_t* ownerId, const uint8_t* adminCapability8) {
  if (!key32) return false;
  int idx = findIndex(groupId);
  if (idx < 0) {
    if (!addGroup(groupId)) return false;
    idx = findIndex(groupId);
    if (idx < 0) return false;
  }
  const uint8_t* selfId = node::getId();
  if (hasPrivateKeyAt(idx) && !canRotateGroupKey(groupId, selfId, adminCapability8)) {
    return false;
  }
  setKeyAt(idx, key32);
  if (ownerId && !node::isInvalidNodeId(ownerId) && !node::isBroadcast(ownerId)) {
    setOwnerAt(idx, ownerId);
  } else if (!hasOwnerAtInternal(idx)) {
    setOwnerAt(idx, selfId);
  }
  if (adminCapability8) setAdminCapAt(idx, adminCapability8);
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

bool clearGroupKey(uint32_t groupId, const uint8_t* adminCapability8) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  if (!canRotateGroupKey(groupId, node::getId(), adminCapability8)) return false;
  clearKeyAt(idx);
  clearOwnerAt(idx);
  clearAdminCapAt(idx);
  persist();
  return true;
}

bool hasGroupKey(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  return hasPrivateKeyAt(idx);
}

bool getGroupKey(uint32_t groupId, uint8_t* out32) {
  if (!out32) return false;
  int idx = findIndex(groupId);
  if (idx < 0 || !hasPrivateKeyAt(idx)) return false;
  memcpy(out32, s_groupKeys[idx], 32);
  return true;
}

uint16_t getGroupKeyVersion(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0 || !hasPrivateKeyAt(idx)) return 0;
  return s_groupKeyVersion[idx];
}

bool getGroupOwner(uint32_t groupId, uint8_t* outOwner8) {
  if (!outOwner8) return false;
  int idx = findIndex(groupId);
  if (idx < 0 || !hasOwnerAtInternal(idx)) return false;
  memcpy(outOwner8, s_groupOwners[idx], protocol::NODE_ID_LEN);
  return true;
}

bool isGroupOwner(uint32_t groupId, const uint8_t* nodeId) {
  if (!nodeId) return false;
  int idx = findIndex(groupId);
  if (idx < 0 || !hasOwnerAtInternal(idx)) return false;
  return memcmp(s_groupOwners[idx], nodeId, protocol::NODE_ID_LEN) == 0;
}

bool hasGroupAdminCapability(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  return hasAdminCapAtInternal(idx);
}

bool setGroupAdminCapability(uint32_t groupId, const uint8_t* adminCapability8) {
  if (!adminCapability8) return false;
  int idx = findIndex(groupId);
  if (idx < 0 || !hasPrivateKeyAt(idx)) return false;
  setAdminCapAt(idx, adminCapability8);
  persist();
  return true;
}

bool clearGroupAdminCapability(uint32_t groupId) {
  int idx = findIndex(groupId);
  if (idx < 0) return false;
  clearAdminCapAt(idx);
  persist();
  return true;
}

bool isPrivateAt(int index) {
  return hasPrivateKeyAt(index);
}

uint16_t keyVersionAt(int index) {
  if (index < 0 || index >= s_count || !hasPrivateKeyAt(index)) return 0;
  return s_groupKeyVersion[index];
}

bool hasOwnerAt(int index) {
  return hasOwnerAtInternal(index);
}

bool canRotateAt(int index) {
  if (index < 0 || index >= s_count) return false;
  if (!hasPrivateKeyAt(index)) return false;
  return canRotateGroupKey(s_groups[index], node::getId(), nullptr) ||
         (hasGroupAdminCapability(s_groups[index]) &&
          canRotateGroupKey(s_groups[index], node::getId(), s_groupAdminCaps[index]));
}

}  // namespace groups
