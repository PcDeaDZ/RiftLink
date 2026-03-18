/**
 * RiftLink Groups — подписка на группы
 */

#include "groups.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_GROUPS "grps"

static uint32_t s_groups[MAX_GROUPS];
static int s_count = 0;
static bool s_inited = false;

namespace groups {

void init() {
  if (s_inited) return;
  memset(s_groups, 0, sizeof(s_groups));
  s_count = 0;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(s_groups);
    if (nvs_get_blob(h, NVS_KEY_GROUPS, s_groups, &len) == ESP_OK) {
      s_count = len / sizeof(uint32_t);
      if (s_count > MAX_GROUPS) s_count = MAX_GROUPS;
    }
    nvs_close(h);
  }
  if (s_count == 0) {
    s_groups[0] = GROUP_ALL;
    s_count = 1;
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
  if (isInGroup(groupId)) return true;
  if (s_count >= MAX_GROUPS) return false;

  s_groups[s_count++] = groupId;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, NVS_KEY_GROUPS, s_groups, s_count * sizeof(uint32_t));
    nvs_commit(h);
    nvs_close(h);
  }
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
      for (int j = i; j < s_count - 1; j++) s_groups[j] = s_groups[j + 1];
      s_count--;
      nvs_handle_t h;
      if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_GROUPS, s_groups, s_count * sizeof(uint32_t));
        nvs_commit(h);
        nvs_close(h);
      }
      return;
    }
  }
}

}  // namespace groups
