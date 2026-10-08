# Rune — Project Context for Claude Code

## Stack
- C++17+ with CMake
- Qt 6.11 (QML / Qt Quick) — installed system-wide, no pkg-config issues
- Quickshell 0.3.1 — Omarchy's QML shell framework
- LibreOffice 26.8 with SDK — headless document engine

## LibreOfficeKit
- Headers: `/usr/include/LibreOfficeKit/` (LibreOfficeKit.h, LibreOfficeKit.hxx, LibreOfficeKitInit.h, LibreOfficeKitTypes.h, LibreOfficeKitEnums.h)
- Library: `/usr/lib/libreoffice/program/liblibreofficekitgtk.so`
- SDK: `/usr/lib/libreoffice/sdk/`
- Office home: `/usr/lib/libreoffice/`
- Source `setsdkenv_unix.sh` script for build environment

## Theming
- Use Omarchy theming: dark theme, system colors from the Omarchy palette
- QML components should follow Omarchy's visual conventions

## Build Pattern
- C++ engine bridges communicate with QML frontend via JSON over Unix sockets or subprocess stdout
- Keep the engine testable headlessly