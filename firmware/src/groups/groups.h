/**
 * RiftLink Groups V2 — thin-device runtime state.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#define GROUP_ID_LEN 4
#define MAX_GROUPS 4

namespace groups {

enum class GroupRole : uint8_t {
  None = 0,
  Member = 1,
  Admin = 2,
  Owner = 3,
};

void init();
/** Служебный id широковещательных OP_GROUP_MSG в mesh; не хранится в списке подписок пользователя */
constexpr uint32_t GROUP_ALL = 1;

// --- Groups V2 thin-device runtime state ---
// V2 хранит только минимальный security-runtime state (без полной истории участников/grants).
constexpr int MAX_GROUPS_V2 = MAX_GROUPS;
constexpr int GROUP_UID_MAX_LEN = 64;
constexpr int GROUP_TAG_MAX_LEN = 32;
constexpr int GROUP_CANONICAL_NAME_MAX_LEN = 48;
constexpr int GROUP_OWNER_SIGN_PUBKEY_LEN = 32;

/** Upsert минимального V2-состояния группы на устройстве. */
bool upsertGroupV2(
    const char* groupUid,
    uint32_t channelId32,
    const char* groupTag,
    const char* canonicalName,
    const uint8_t* groupKey32,
    uint16_t keyVersion,
    GroupRole myRole,
    uint32_t revocationEpoch);
/** Обновить canonicalName (разрешено owner-only в BLE handler). */
bool setCanonicalNameV2(const char* groupUid, const char* canonicalName);
/** Зафиксировать owner signing public key (32B) для группы. */
bool setOwnerSignPubKeyV2(const char* groupUid, const uint8_t* ownerSignPubKey32);
/** Прочитать owner signing public key (32B) для группы. */
bool getOwnerSignPubKeyV2(const char* groupUid, uint8_t* outOwnerSignPubKey32);
/** Установить локальную роль узла в группе V2. */
bool setGroupRoleV2(const char* groupUid, GroupRole role);
/** Обновить ключ/версию ключа в группе V2 (rekey). */
bool updateGroupKeyV2(const char* groupUid, const uint8_t* groupKey32, uint16_t keyVersion);
/** Подтвердить применение ключа на устройстве (ACK_KEY_APPLIED локально). */
bool ackKeyAppliedV2(const char* groupUid, uint16_t keyVersion);
/** Обновить watermark отзыва прав. */
bool setRevocationEpochV2(const char* groupUid, uint32_t revocationEpoch);
/** Удалить V2-группу из локального runtime/NVS состояния. */
bool removeGroupV2(const char* groupUid);
/** Получить V2-состояние по groupUid. */
bool getGroupV2(
    const char* groupUid,
    uint32_t* outChannelId32,
    char* outGroupTag,
    size_t outGroupTagLen,
    char* outCanonicalName,
    size_t outCanonicalNameLen,
    uint16_t* outKeyVersion,
    GroupRole* outRole,
    uint32_t* outRevocationEpoch,
    bool* outAckApplied);
/** Получить ключ и версию ключа V2-группы. */
bool getGroupKeyV2(const char* groupUid, uint8_t* outKey32, uint16_t* outKeyVersion);
/** Получить ключ V2-группы по transport channelId32. */
bool getGroupKeyV2ByChannel(uint32_t channelId32, uint8_t* outKey32, uint16_t* outKeyVersion);
/** Найти groupUid по transport channelId32. */
bool findGroupUidByChannelV2(uint32_t channelId32, char* outGroupUid, size_t outGroupUidLen);
/** Количество V2-групп в runtime state. */
int getV2Count();
/** Получить V2-запись по индексу. */
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
    bool* outAckApplied);

}  // namespace groups
