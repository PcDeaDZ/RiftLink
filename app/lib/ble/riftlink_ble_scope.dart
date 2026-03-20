import 'package:flutter/widgets.dart';

import 'riftlink_ble.dart';

/// Один экземпляр [RiftLinkBle] на всё приложение — выше [MaterialApp] и [ListenableBuilder],
/// чтобы пересборка темы/локали не подменяла объект с активным GATT RX/подписками.
class RiftLinkBleScope extends InheritedWidget {
  const RiftLinkBleScope({
    super.key,
    required this.ble,
    required super.child,
  });

  final RiftLinkBle ble;

  static RiftLinkBle of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<RiftLinkBleScope>();
    assert(scope != null, 'RiftLinkBleScope не найден над context');
    return scope!.ble;
  }

  @override
  bool updateShouldNotify(RiftLinkBleScope oldWidget) => !identical(oldWidget.ble, ble);
}
