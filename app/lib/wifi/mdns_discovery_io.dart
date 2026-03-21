import 'dart:async';
import 'dart:io';

import 'package:multicast_dns/multicast_dns.dart';

import 'mdns_discovery.dart';

const String _riftlinkService = '_riftlink._tcp.local';

Future<List<T>> _collectFor<T>(Stream<T> stream, Duration duration) async {
  final out = <T>[];
  final done = Completer<void>();
  late final StreamSubscription<T> sub;
  Timer? timer;
  sub = stream.listen(
    out.add,
    onError: (_) {},
    onDone: () {
      if (!done.isCompleted) done.complete();
    },
  );
  timer = Timer(duration, () {
    unawaited(sub.cancel());
    if (!done.isCompleted) done.complete();
  });
  await done.future;
  await sub.cancel();
  timer.cancel();
  return out;
}

Future<List<MdnsNode>> discoverRiftLinkMdns() async {
  Future<RawDatagramSocket> socketFactory(
    dynamic host,
    int port, {
    bool reuseAddress = true,
    bool reusePort = true,
    int ttl = 1,
  }) async {
    try {
      return await RawDatagramSocket.bind(
        host,
        port,
        reuseAddress: reuseAddress,
        reusePort: reusePort,
        ttl: ttl,
      );
    } catch (_) {
      // Some Android stacks do not support reusePort for UDP multicast sockets.
      return RawDatagramSocket.bind(
        host,
        port,
        reuseAddress: reuseAddress,
        reusePort: false,
        ttl: ttl,
      );
    }
  }

  final client = MDnsClient(rawDatagramSocketFactory: socketFactory);
  final found = <String, MdnsNode>{};
  try {
    await client.start();
    final ptrs = await _collectFor<PtrResourceRecord>(
      client.lookup<PtrResourceRecord>(
        ResourceRecordQuery.serverPointer(_riftlinkService),
      ),
      const Duration(seconds: 2),
    );
    for (final ptr in ptrs) {
      final srvs = await _collectFor<SrvResourceRecord>(
        client.lookup<SrvResourceRecord>(
          ResourceRecordQuery.service(ptr.domainName),
        ),
        const Duration(milliseconds: 800),
      );
      for (final srv in srvs) {
        final ips = await _collectFor<IPAddressResourceRecord>(
          client.lookup<IPAddressResourceRecord>(
            ResourceRecordQuery.addressIPv4(srv.target),
          ),
          const Duration(milliseconds: 800),
        );
        for (final rec in ips) {
          final ip = rec.address.address;
          final key = '$ip:${srv.port}';
          found[key] = MdnsNode(host: srv.target, ip: ip, port: srv.port);
        }
      }
    }
  } finally {
    client.stop();
  }
  return found.values.toList();
}
