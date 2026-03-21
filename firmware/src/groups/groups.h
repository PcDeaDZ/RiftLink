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
bool setGroupKey(uint32_t groupId, const uint8_t* key32);
/** Удалить приватный ключ группы (группа становится public). */
bool clearGroupKey(uint32_t groupId);
/** Есть ли приватный ключ у группы. */
bool hasGroupKey(uint32_t groupId);
/** Получить приватный ключ группы. */
bool getGroupKey(uint32_t groupId, uint8_t* out32);
/** Количество групп */
int getCount();
/** ID группы по индексу (0..getCount()-1) */
uint32_t getId(int index);
/** Наличие приватного ключа у группы по индексу. */
bool isPrivateAt(int index);
/** Служебный id широковещательных OP_GROUP_MSG в mesh; не хранится в списке подписок пользователя */
constexpr uint32_t GROUP_ALL = 1;

}  // namespace groups
