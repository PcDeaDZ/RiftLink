import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/screens/chat/widgets/chat_app_bar.dart';
import 'package:riftlink_app/theme/app_theme.dart';

void main() {
  testWidgets('chat app bar title renders chat icon and subtitle', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: ThemeData(extensions: const <ThemeExtension<dynamic>>[AppPalette.dark]),
        home: Scaffold(
          body: ChatAppBarTitle(
            chatIcon: Icons.group_outlined,
            label: 'Team Alpha',
            subtitle: 'Group 42 · Admin',
          ),
        ),
      ),
    );

    expect(find.text('Team Alpha'), findsOneWidget);
    expect(find.text('Group 42 · Admin'), findsOneWidget);
    expect(find.byIcon(Icons.group_outlined), findsOneWidget);
    expect(find.byIcon(Icons.bluetooth_connected), findsNothing);
  });
}
