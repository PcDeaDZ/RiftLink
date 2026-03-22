/**
 * RiftLink Groups — подписка на групповые каналы
 * GROUP_MSG (0x09): payload = [group_id 4B][text]
 */

#pragma once

#include <cstdint>

#define GROUP_ID_LEN 4
#define MAX_GROUPS 4

namespace groups {

void init();
/** Проверить, подписан ли узел на группу */
bool isInGroup(uint32_t groupId);
/** Добавить группу (до MAX_GROUPS) */
bool addGroup(uint32_t groupId);
/** Удалить группу */
void removeGroup(uint32_t groupId);
/** Установить/обновить приватный ключ группы (32 байта). */
bool setGroupKey(
    uint32_t groupId,
    const uint8_t* key32,
    uint16_t keyVersion = 0,
    const uint8_t* ownerId = nullptr,
    const uint8_t* adminCapability8 = nullptr);
/** Удалить приватный ключ группы (группа становится public). */
bool clearGroupKey(uint32_t groupId, const uint8_t* adminCapability8 = nullptr);
/** Есть ли приватный ключ у группы. */
bool hasGroupKey(uint32_t groupId);
/** Получить приватный ключ группы. */
bool getGroupKey(uint32_t groupId, uint8_t* out32);
/** Текущая версия ключа группы (0 если ключ не задан). */
uint16_t getGroupKeyVersion(uint32_t groupId);
/** Получить owner группы (8 байт NodeID). */
bool getGroupOwner(uint32_t groupId, uint8_t* outOwner8);
/** Текущий узел является owner группы. */
bool isGroupOwner(uint32_t groupId, const uint8_t* nodeId);
/** Есть ли локально сохранённая admin-capability (8 байт) для группы. */
bool hasGroupAdminCapability(uint32_t groupId);
/** Сохранить admin-capability для группы (полученную отдельным admin-кодом). */
bool setGroupAdminCapability(uint32_t groupId, const uint8_t* adminCapability8);
/** Удалить локальную admin-capability группы. */
bool clearGroupAdminCapability(uint32_t groupId);
/** Разрешена ли ротация ключа для requester (owner либо корректная admin-capability). */
bool canRotateGroupKey(uint32_t groupId, const uint8_t* requesterId, const uint8_t* presentedAdminCapability8 = nullptr);
/** Количество групп */
int getCount();
/** ID группы по индексу (0..getCount()-1) */
uint32_t getId(int index);
/** Наличие приватного ключа у группы по индексу. */
bool isPrivateAt(int index);
/** Версия ключа группы по индексу. */
uint16_t keyVersionAt(int index);
/** Есть ли owner у группы по индексу. */
bool hasOwnerAt(int index);
/** Может ли текущий узел ротировать ключ группы по индексу (owner/admin-capability). */
bool canRotateAt(int index);
/** Служебный id широковещательных OP_GROUP_MSG в mesh; не хранится в списке подписок пользователя */
constexpr uint32_t GROUP_ALL = 1;

}  // namespace groups
