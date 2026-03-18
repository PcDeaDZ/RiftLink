/// Диагностика: если этот экран тоже серый — проблема в Flutter/Android, не в наших виджетах.
import 'package:flutter/material.dart';

class DebugScreen extends StatelessWidget {
  const DebugScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.red,
      child: SafeArea(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Text('ТЕСТ', style: TextStyle(fontSize: 48, color: Colors.white, fontWeight: FontWeight.bold)),
            const SizedBox(height: 16),
            const Text('Если видишь красный — Flutter рендерит нормально', textAlign: TextAlign.center, style: TextStyle(fontSize: 16, color: Colors.white)),
            const SizedBox(height: 32),
            TextButton(
              onPressed: () => Navigator.pop(context),
              style: TextButton.styleFrom(foregroundColor: Colors.white, backgroundColor: Colors.black54),
              child: const Text('Назад'),
            ),
          ],
        ),
      ),
    );
  }
}
