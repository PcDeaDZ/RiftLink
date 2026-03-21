import 'mdns_discovery_stub.dart'
    if (dart.library.io) 'mdns_discovery_io.dart' as impl;

class MdnsNode {
  final String host;
  final String ip;
  final int port;

  const MdnsNode({
    required this.host,
    required this.ip,
    required this.port,
  });
}

Future<List<MdnsNode>> discoverRiftLinkMdns() => impl.discoverRiftLinkMdns();
