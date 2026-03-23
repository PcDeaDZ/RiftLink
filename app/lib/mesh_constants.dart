import 'dart:math';

/// Совпадает с `groups::GROUP_ALL` в прошивке: служебный id для OP_GROUP_MSG broadcast, не пользовательская подписка.
const int kMeshBroadcastGroupId = 1;

/// Случайный `channelId32` для новой группы (2…0xFFFFFFFF), не пересекающийся с [occupied].
/// [occupied] должен содержать как минимум [kMeshBroadcastGroupId] при необходимости.
int pickRandomGroupChannelId32(Set<int> occupied) {
  final r = Random.secure();
  for (var attempt = 0; attempt < 64; attempt++) {
    final v = 2 + r.nextInt(0xFFFFFFFE);
    if (v == kMeshBroadcastGroupId) continue;
    if (!occupied.contains(v)) return v;
  }
  var v = DateTime.now().microsecondsSinceEpoch & 0xFFFFFFFF;
  if (v <= 1) v = 2;
  for (var i = 0; i < 0x100000; i++) {
    if (!occupied.contains(v)) return v;
    v = (v >= 0xFFFFFFFF) ? 2 : v + 1;
  }
  return 2;
}
