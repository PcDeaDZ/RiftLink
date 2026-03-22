package com.riftlink.app

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "riftlink/app_lifecycle"
        ).setMethodCallHandler { call, result ->
            when (call.method) {
                "moveToBackground" -> result.success(moveTaskToBack(true))
                else -> result.notImplemented()
            }
        }
    }
}
