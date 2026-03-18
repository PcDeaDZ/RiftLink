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
/** Количество групп */
int getCount();
/** ID группы по индексу (0..getCount()-1) */
uint32_t getId(int index);
/** Группа по умолчанию (все) */
constexpr uint32_t GROUP_ALL = 1;

}  // namespace groups
