import 'dart:async';

import 'package:flutter/material.dart';
import '../widgets/mesh_background.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:geolocator/geolocator.dart';
import 'package:latlong2/latlong.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';

const _blue = Color(0xFF1565C0);

class MapScreen extends StatefulWidget {
  final RiftLinkBle ble;
  const MapScreen({super.key, required this.ble});
  @override
  State<MapScreen> createState() => _MapScreenState();
}

class _MapScreenState extends State<MapScreen> {
  final Map<String, _NL> _nodes = {};
  StreamSubscription<RiftLinkEvent>? _sub;
  LatLng? _myLocation;
  bool _centerOnSelf = false;
  /// Счётчик: без MapController `initialCenter` не обновляется; при каждом запросе геолокации пересоздаём [FlutterMap].
  int _mapRecenterEpoch = 0;

  @override
  void initState() {
    super.initState();
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkLocationEvent) setState(() { _nodes[evt.from] = _NL(lat: evt.lat, lon: evt.lon, alt: evt.alt); });
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final points = _nodes.entries.map((e) => LatLng(e.value.lat, e.value.lon)).toList();
    if (_myLocation != null) points.add(_myLocation!);

    var center = const LatLng(55.7558, 37.6173);
    if (_centerOnSelf && _myLocation != null) {
      center = _myLocation!;
    } else if (points.isNotEmpty) {
      double sLat = 0, sLon = 0;
      for (final p in points) { sLat += p.latitude; sLon += p.longitude; }
      center = LatLng(sLat / points.length, sLon / points.length);
    }

    return Scaffold(
      backgroundColor: Colors.white,
      appBar: AppBar(
        title: Text(context.l10n.tr('map')),
        leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context)),
        actions: [
          IconButton(
            icon: const Icon(Icons.radar),
            tooltip: context.l10n.tr('geofence_send'),
            onPressed: () async {
              try {
                final pos = await Geolocator.getCurrentPosition();
                final expiry = DateTime.now().add(const Duration(minutes: 30)).millisecondsSinceEpoch ~/ 1000;
                await widget.ble.sendLocation(
                  lat: pos.latitude,
                  lon: pos.longitude,
                  alt: pos.altitude.toInt(),
                  radiusM: 300,
                  expiryEpochSec: expiry,
                );
                if (!mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text(context.l10n.tr('geofence_sent'))),
                );
              } catch (_) {}
            },
          ),
          IconButton(icon: const Icon(Icons.my_location), tooltip: context.l10n.tr('map_my_location'), onPressed: () async {
            try {
              final pos = await Geolocator.getCurrentPosition();
              setState(() {
                _myLocation = LatLng(pos.latitude, pos.longitude);
                _centerOnSelf = true;
                _mapRecenterEpoch++;
              });
            } catch (_) {}
          }),
        ],
      ),
      body: MeshBackgroundWrapper(
        child: Material(
          color: Colors.transparent,
          child: FlutterMap(
        // `initialCenter` / zoom применяются только при создании виджета; epoch гарантирует пересоздание даже при совпадающих координатах.
        key: ValueKey('map_${_mapRecenterEpoch}_${_myLocation?.latitude}_${_myLocation?.longitude}'),
        options: MapOptions(initialCenter: center, initialZoom: points.isEmpty ? 10 : 14),
        children: [
          TileLayer(urlTemplate: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}'),
          MarkerLayer(markers: [
            ..._nodes.entries.map((e) => Marker(
              point: LatLng(e.value.lat, e.value.lon), width: 80, height: 44,
              child: Tooltip(message: e.key, child: Column(mainAxisSize: MainAxisSize.min, children: [
                const Icon(Icons.radio_button_checked, color: _blue, size: 32),
                Text(e.key.length > 8 ? e.key.substring(0, 8) : e.key, style: const TextStyle(fontSize: 10, fontFamily: 'monospace', color: Colors.black87), overflow: TextOverflow.ellipsis),
              ])),
            )),
            if (_myLocation != null) Marker(point: _myLocation!, width: 40, height: 40, child: const Icon(Icons.person_pin_circle, color: Color(0xFF388E3C), size: 40)),
          ]),
        ],
      ),
        ),
        ),
      bottomSheet: _nodes.isEmpty && _myLocation == null
        ? Container(
            width: double.infinity, padding: const EdgeInsets.all(24), color: Colors.white,
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Icon(Icons.location_off, size: 48, color: Colors.grey.shade400),
              const SizedBox(height: 12),
              Text(context.l10n.tr('map_waiting'), textAlign: TextAlign.center, style: const TextStyle(color: Color(0xFF757575), fontSize: 14)),
            ]),
          )
        : null,
    );
  }
}

class _NL {
  final double lat, lon;
  final int alt;
  _NL({required this.lat, required this.lon, required this.alt});
}
