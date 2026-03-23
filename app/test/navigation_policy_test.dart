import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/app_navigator.dart';

void main() {
  testWidgets('back behavior: chat -> home', (tester) async {
    await tester.pumpWidget(const _NavHarness());

    await tester.tap(find.byKey(const Key('open_chat')));
    await tester.pumpAndSettle();
    // _SimplePage shows title in AppBar and body.
    expect(find.text('chat'), findsNWidgets(2));

    await tester.pageBack();
    await tester.pumpAndSettle();
    expect(find.text('home'), findsOneWidget);
  });

  testWidgets('open chat from drawer', (tester) async {
    await tester.pumpWidget(const _NavHarness());

    await tester.tap(find.byTooltip('Open navigation menu'));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('drawer_open_chat')));
    await tester.pumpAndSettle();

    expect(find.text('chat'), findsNWidgets(2));
  });

  testWidgets('disconnect redirect resets to scan', (tester) async {
    await tester.pumpWidget(const _NavHarness());

    await tester.tap(find.byKey(const Key('disconnect')));
    await tester.pumpAndSettle();

    expect(find.text('scan'), findsNWidgets(2));
    expect(find.text('home'), findsNothing);
  });

  testWidgets('reconnect-failed redirect resets to scan-error', (tester) async {
    await tester.pumpWidget(const _NavHarness());

    await tester.tap(find.byKey(const Key('reconnect_failed')));
    await tester.pumpAndSettle();

    expect(find.text('scan_error'), findsNWidgets(2));
    expect(find.text('home'), findsNothing);
  });
}

class _NavHarness extends StatelessWidget {
  const _NavHarness();

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      navigatorKey: navigatorKey,
      home: const _HomePage(),
    );
  }
}

class _HomePage extends StatelessWidget {
  const _HomePage();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('home')),
      drawer: Drawer(
        child: SafeArea(
          child: ListTile(
            key: const Key('drawer_open_chat'),
            title: const Text('Open chat'),
            onTap: () {
              Navigator.pop(context);
              appPush(context, const _SimplePage('chat'));
            },
          ),
        ),
      ),
      body: Column(
        children: [
          ElevatedButton(
            key: const Key('open_chat'),
            onPressed: () => appPush(context, const _SimplePage('chat')),
            child: const Text('open_chat'),
          ),
          ElevatedButton(
            key: const Key('disconnect'),
            onPressed: () => appResetTo(context, const _SimplePage('scan')),
            child: const Text('disconnect'),
          ),
          ElevatedButton(
            key: const Key('reconnect_failed'),
            onPressed: () => appResetTo(context, const _SimplePage('scan_error')),
            child: const Text('reconnect_failed'),
          ),
        ],
      ),
    );
  }
}

class _SimplePage extends StatelessWidget {
  final String title;

  const _SimplePage(this.title);

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(title)),
      body: Center(child: Text(title)),
    );
  }
}
