/**
 * RiftLink Groups V2 runtime state (legacy V1 removed).
 */

#include "groups.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_GROUPS_V2 "gr2s"
#define NVS_KEY_GROUPS_V2_CNT "gr2c"
// Legacy V1 NVS keys wiped on V2-only init.
#define NVS_KEY_GROUPS "grps"
#define NVS_KEY_GROUP_KEYS "grpk"
#define NVS_KEY_GROUP_KEY_MASK "grpm"
#define NVS_KEY_GROUP_KEY_VER "grpv"
#define NVS_KEY_GROUP_OWNERS "grpo"
#define NVS_KEY_GROUP_OWNER_MASK "grpom"
#define NVS_KEY_GROUP_CAPS "grpc"
#define NVS_KEY_GROUP_CAP_MASK "grpcm"

namespace groups {

struct GroupV2Slot {
  char groupUid[GROUP_UID_MAX_LEN + 1];
  uint32_t channelId32;
  char groupTag[GROUP_TAG_MAX_LEN + 1];
  char canonicalName[GROUP_CANONICAL_NAME_MAX_LEN + 1];
  uint8_t ownerSignPubKey[GROUP_OWNER_SIGN_PUBKEY_LEN];
  uint8_t groupKey[32];
  uint16_t keyVersion;
  uint8_t role;
  uint32_t revocationEpoch;
  uint8_t ackApplied;
  uint8_t hasOwnerSignPubKey;
};

static GroupV2Slot s_groupsV2[MAX_GROUPS_V2];
static int s_countV2 = 0;
static bool s_inited = false;

static void copySafe(char* dst, size_t dstLen, const char* src) {
  if (!dst || dstLen == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const size_t n = strnlen(src, dstLen - 1);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static uint8_t roleToByte(GroupRole role) {
  if (role == GroupRole::Owner) return 3;
  if (role == GroupRole::Admin) return 2;
  if (role == GroupRole::Member) return 1;
  return 0;
}

static GroupRole byteToRole(uint8_t value) {
  if (value == 3) return GroupRole::Owner;
  if (value == 2) return GroupRole::Admin;
  if (value == 1) return GroupRole::Member;
  return GroupRole::None;
}

static int findV2Index(const char* groupUid) {
  if (!groupUid || !groupUid[0]) return -1;
  for (int i = 0; i < s_countV2; i++) {
    if (strncmp(s_groupsV2[i].groupUid, groupUid, GROUP_UID_MAX_LEN) == 0) return i;
  }
  return -1;
}

static int findV2IndexByChannel(uint32_t channelId32) {
  if (channelId32 == 0) return -1;
  for (int i = 0; i < s_countV2; i++) {
    if (s_groupsV2[i].channelId32 == channelId32) return i;
  }
  return -1;
}

static void persistV2() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, NVS_KEY_GROUPS_V2, s_groupsV2, sizeof(s_groupsV2));
  nvs_set_u8(h, NVS_KEY_GROUPS_V2_CNT, (uint8_t)s_countV2);
  nvs_commit(h);
  nvs_close(h);
}

void init() {
  if (s_inited) return;
  memset(s_groupsV2, 0, sizeof(s_groupsV2));
  s_countV2 = 0;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    size_t groupsV2Len = sizeof(s_groupsV2);
    if (nvs_get_blob(h, NVS_KEY_GROUPS_V2, s_groupsV2, &groupsV2Len) != ESP_OK || groupsV2Len != sizeof(s_groupsV2)) {
      memset(s_groupsV2, 0, sizeof(s_groupsV2));
    }
    uint8_t groupsV2Cnt = 0;
    if (nvs_get_u8(h, NVS_KEY_GROUPS_V2_CNT, &groupsV2Cnt) == ESP_OK) {
      s_countV2 = (groupsV2Cnt > MAX_GROUPS_V2) ? MAX_GROUPS_V2 : (int)groupsV2Cnt;
    }

    // Hard purge of persisted V1 group state.
    nvs_erase_key(h, NVS_KEY_GROUPS);
    nvs_erase_key(h, NVS_KEY_GROUP_KEYS);
    nvs_erase_key(h, NVS_KEY_GROUP_KEY_MASK);
    nvs_erase_key(h, NVS_KEY_GROUP_KEY_VER);
    nvs_erase_key(h, NVS_KEY_GROUP_OWNERS);
    nvs_erase_key(h, NVS_KEY_GROUP_OWNER_MASK);
    nvs_erase_key(h, NVS_KEY_GROUP_CAPS);
    nvs_erase_key(h, NVS_KEY_GROUP_CAP_MASK);
    nvs_commit(h);
    nvs_close(h);
  }

  s_inited = true;
}

bool upsertGroupV2(const char* groupUid, uint32_t channelId32, const char* groupTag, const char* canonicalName, const uint8_t* groupKey32, uint16_t keyVersion, GroupRole myRole, uint32_t revocationEpoch) {
  if (!groupUid || !groupUid[0] || !groupKey32) return false;
  int idx = findV2Index(groupUid);
  if (idx < 0) {
    if (s_countV2 >= MAX_GROUPS_V2) return false;
    idx = s_countV2++;
    memset(&s_groupsV2[idx], 0, sizeof(s_groupsV2[idx]));
  }

  const uint16_t prevKeyVersion = s_groupsV2[idx].keyVersion;
  copySafe(s_groupsV2[idx].groupUid, sizeof(s_groupsV2[idx].groupUid), groupUid);
  copySafe(s_groupsV2[idx].groupTag, sizeof(s_groupsV2[idx].groupTag), groupTag);
  copySafe(s_groupsV2[idx].canonicalName, sizeof(s_groupsV2[idx].canonicalName), canonicalName);
  s_groupsV2[idx].channelId32 = channelId32;
  memcpy(s_groupsV2[idx].groupKey, groupKey32, 32);
  s_groupsV2[idx].keyVersion = keyVersion;
  s_groupsV2[idx].role = roleToByte(myRole);
  s_groupsV2[idx].revocationEpoch = revocationEpoch;
  if (prevKeyVersion != keyVersion) s_groupsV2[idx].ackApplied = 0;

  persistV2();
  return true;
}

bool setCanonicalNameV2(const char* groupUid, const char* canonicalName) {
  int idx = findV2Index(groupUid);
  if (idx < 0 || !canonicalName || !canonicalName[0]) return false;
  copySafe(s_groupsV2[idx].canonicalName, sizeof(s_groupsV2[idx].canonicalName), canonicalName);
  persistV2();
  return true;
}

bool setOwnerSignPubKeyV2(const char* groupUid, const uint8_t* ownerSignPubKey32) {
  int idx = findV2Index(groupUid);
  if (idx < 0 || !ownerSignPubKey32) return false;
  memcpy(s_groupsV2[idx].ownerSignPubKey, ownerSignPubKey32, GROUP_OWNER_SIGN_PUBKEY_LEN);
  s_groupsV2[idx].hasOwnerSignPubKey = 1;
  persistV2();
  return true;
}

bool getOwnerSignPubKeyV2(const char* groupUid, uint8_t* outOwnerSignPubKey32) {
  int idx = findV2Index(groupUid);
  if (idx < 0 || !outOwnerSignPubKey32 || s_groupsV2[idx].hasOwnerSignPubKey == 0) return false;
  memcpy(outOwnerSignPubKey32, s_groupsV2[idx].ownerSignPubKey, GROUP_OWNER_SIGN_PUBKEY_LEN);
  return true;
}

bool setGroupRoleV2(const char* groupUid, GroupRole role) {
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  s_groupsV2[idx].role = roleToByte(role);
  persistV2();
  return true;
}

bool updateGroupKeyV2(const char* groupUid, const uint8_t* groupKey32, uint16_t keyVersion) {
  if (!groupKey32) return false;
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  memcpy(s_groupsV2[idx].groupKey, groupKey32, 32);
  s_groupsV2[idx].keyVersion = keyVersion;
  s_groupsV2[idx].ackApplied = 0;
  persistV2();
  return true;
}

bool ackKeyAppliedV2(const char* groupUid, uint16_t keyVersion) {
  int idx = findV2Index(groupUid);
  if (idx < 0 || s_groupsV2[idx].keyVersion != keyVersion) return false;
  s_groupsV2[idx].ackApplied = 1;
  persistV2();
  return true;
}

bool setRevocationEpochV2(const char* groupUid, uint32_t revocationEpoch) {
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  s_groupsV2[idx].revocationEpoch = revocationEpoch;
  persistV2();
  return true;
}

bool removeGroupV2(const char* groupUid) {
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  for (int i = idx; i < s_countV2 - 1; i++) {
    s_groupsV2[i] = s_groupsV2[i + 1];
  }
  if (s_countV2 > 0) {
    s_countV2--;
    memset(&s_groupsV2[s_countV2], 0, sizeof(s_groupsV2[s_countV2]));
  }
  persistV2();
  return true;
}

bool getGroupV2(const char* groupUid, uint32_t* outChannelId32, char* outGroupTag, size_t outGroupTagLen, char* outCanonicalName, size_t outCanonicalNameLen, uint16_t* outKeyVersion, GroupRole* outRole, uint32_t* outRevocationEpoch, bool* outAckApplied) {
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  if (outChannelId32) *outChannelId32 = s_groupsV2[idx].channelId32;
  if (outGroupTag && outGroupTagLen > 0) copySafe(outGroupTag, outGroupTagLen, s_groupsV2[idx].groupTag);
  if (outCanonicalName && outCanonicalNameLen > 0) copySafe(outCanonicalName, outCanonicalNameLen, s_groupsV2[idx].canonicalName);
  if (outKeyVersion) *outKeyVersion = s_groupsV2[idx].keyVersion;
  if (outRole) *outRole = byteToRole(s_groupsV2[idx].role);
  if (outRevocationEpoch) *outRevocationEpoch = s_groupsV2[idx].revocationEpoch;
  if (outAckApplied) *outAckApplied = (s_groupsV2[idx].ackApplied != 0);
  return true;
}

bool getGroupKeyV2(const char* groupUid, uint8_t* outKey32, uint16_t* outKeyVersion) {
  if (!outKey32) return false;
  int idx = findV2Index(groupUid);
  if (idx < 0) return false;
  memcpy(outKey32, s_groupsV2[idx].groupKey, 32);
  if (outKeyVersion) *outKeyVersion = s_groupsV2[idx].keyVersion;
  return true;
}

bool getGroupKeyV2ByChannel(uint32_t channelId32, uint8_t* outKey32, uint16_t* outKeyVersion) {
  if (!outKey32) return false;
  int idx = findV2IndexByChannel(channelId32);
  if (idx < 0) return false;
  memcpy(outKey32, s_groupsV2[idx].groupKey, 32);
  if (outKeyVersion) *outKeyVersion = s_groupsV2[idx].keyVersion;
  return true;
}

bool findGroupUidByChannelV2(uint32_t channelId32, char* outGroupUid, size_t outGroupUidLen) {
  if (!outGroupUid || outGroupUidLen == 0) return false;
  int idx = findV2IndexByChannel(channelId32);
  if (idx < 0) return false;
  copySafe(outGroupUid, outGroupUidLen, s_groupsV2[idx].groupUid);
  return true;
}

int getV2Count() {
  return s_countV2;
}

bool getV2At(
    int index,
    char* outGroupUid,
    size_t outGroupUidLen,
    uint32_t* outChannelId32,
    char* outGroupTag,
    size_t outGroupTagLen,
    char* outCanonicalName,
    size_t outCanonicalNameLen,
    uint16_t* outKeyVersion,
    GroupRole* outRole,
    uint32_t* outRevocationEpoch,
    bool* outAckApplied) {
  if (index < 0 || index >= s_countV2) return false;
  if (outGroupUid && outGroupUidLen > 0) copySafe(outGroupUid, outGroupUidLen, s_groupsV2[index].groupUid);
  if (outChannelId32) *outChannelId32 = s_groupsV2[index].channelId32;
  if (outGroupTag && outGroupTagLen > 0) copySafe(outGroupTag, outGroupTagLen, s_groupsV2[index].groupTag);
  if (outCanonicalName && outCanonicalNameLen > 0) copySafe(outCanonicalName, outCanonicalNameLen, s_groupsV2[index].canonicalName);
  if (outKeyVersion) *outKeyVersion = s_groupsV2[index].keyVersion;
  if (outRole) *outRole = byteToRole(s_groupsV2[index].role);
  if (outRevocationEpoch) *outRevocationEpoch = s_groupsV2[index].revocationEpoch;
  if (outAckApplied) *outAckApplied = (s_groupsV2[index].ackApplied != 0);
  return true;
}

}  // namespace groups
