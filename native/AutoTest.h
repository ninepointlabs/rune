#pragma once

// Headless self-test behind --auto-test: drives the real Main.qml window
// (key events into the TextEdit, toolbar toggles through DocumentController)
// and saves to /tmp/rune-native-stage1.odt. Logs each check; true = all pass.

class QQuickWindow;

bool runAutoTest(QQuickWindow *window);
