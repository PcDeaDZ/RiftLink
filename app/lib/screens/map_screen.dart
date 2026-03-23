import 'dart:async';

import 'package:flutter/material.dart';
import '../widgets/mesh_background.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:geolocator/geolocator.dart';
import 'package:latlong2/latlong.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';

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
    final p = context.palette;
    final points = _nodes.entries.map((e) => LatLng(e.value.lat, e.value.lon)).toList();
    if (_myLocation != null) points.add(_myLocation!);

    var center = const LatLng(55.7558, 37.6173);
    if (_centerOnSelf && _myLocation != null) {
      center = _myLocation!;
    } else if (points.isNotEmpty) {
      double sLat = 0, sLon = 0;
      for (final pt in points) { sLat += pt.latitude; sLon += pt.longitude; }
      center = LatLng(sLat / points.length, sLon / points.length);
    }

    return Scaffold(
      backgroundColor: p.surface,
      appBar: riftAppBar(
        context,
        title: context.l10n.tr('map'),
        showBack: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.radar),
            iconSize: AppIconSize.md,
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
          IconButton(
            icon: const Icon(Icons.my_location),
            iconSize: AppIconSize.md,
            tooltip: context.l10n.tr('map_my_location'),
            onPressed: () async {
            try {
              final pos = await Geolocator.getCurrentPosition();
              setState(() {
                _myLocation = LatLng(pos.latitude, pos.longitude);
                _centerOnSelf = true;
                _mapRecenterEpoch++;
              });
            } catch (_) {}
            },
          ),
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
                Icon(Icons.radio_button_checked, color: p.primary, size: AppIconSize.mapMarker),
                Text(
                  e.key,
                  style: AppTypography.captionDenseBase().copyWith(fontFamily: 'monospace', color: p.onSurface),
                  overflow: TextOverflow.ellipsis,
                ),
              ])),
            )),
            if (_myLocation != null)
              Marker(
                point: _myLocation!,
                width: AppIconSize.mapUserPin,
                height: AppIconSize.mapUserPin,
                child: Icon(Icons.person_pin_circle, color: p.success, size: AppIconSize.mapUserPin),
              ),
          ]),
        ],
      ),
        ),
        ),
      bottomSheet: _nodes.isEmpty && _myLocation == null
        ? Container(
            width: double.infinity,
            decoration: BoxDecoration(
              color: p.card,
              border: Border(top: BorderSide(color: p.divider)),
            ),
            padding: const EdgeInsets.all(AppSpacing.xxl),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Icon(Icons.location_off, size: AppIconSize.emptyStateHero, color: p.onSurfaceVariant.withOpacity(0.45)),
              const SizedBox(height: AppSpacing.md),
              Text(
                context.l10n.tr('map_waiting'),
                textAlign: TextAlign.center,
                style: AppTypography.bodyLargeBase().copyWith(color: p.onSurfaceVariant, height: 1.4),
              ),
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
