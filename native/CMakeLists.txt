# native: Rune's implementation. Single-process, built on QML TextEdit +
# QTextDocument, replacing the LibreOfficeKit engine/app pair in legacy-lok/.
# Built automatically as part of the root build:
#
#   cmake -S . -B build -G Ninja && cmake --build build
#
# Or standalone:
#   cmake -S native -B native/build -G Ninja
#   cmake --build native/build

cmake_minimum_required(VERSION 3.21)
project(rune_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 6.7: QQuickTextDocument::setTextDocument(), which DocumentController relies on.
# Test: QTest::keyClick() for --auto-test's real-keyboard shortcut checks.
# Network: the AI providers' sign-in and streaming requests (ai/).
find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Quick Concurrent Network Test)
# ai/: OpenSSL verifies ChatGPT ID-token signatures (RS256); libsecret keeps
# sign-in secrets in the desktop keyring (Secret Service).
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
# Unzipping .odt files for OdfReader. (DocxBridge needs `soffice` at run time
# only, found in PATH.)
find_package(QuaZip-Qt6 REQUIRED)

qt_standard_project_setup(REQUIRES 6.7)

qt_add_executable(rune main.cpp AutoTest.h AutoTest.cpp AiAutoTest.h AiAutoTest.cpp)
# Interpreted, not compiled ahead of time: see the comment at its top.
set_source_files_properties(UnsavedChangesDialog.qml PROPERTIES QT_QML_SKIP_CACHEGEN TRUE)
qt_add_qml_module(rune
    URI Rune
    VERSION 1.0
    QML_FILES Main.qml ToolButton.qml UnsavedChangesDialog.qml AiPanel.qml
    SOURCES DocumentController.h DocumentController.cpp OdfReader.h OdfReader.cpp
            DocxBridge.h DocxBridge.cpp
            ai/AiManager.h ai/AiManager.cpp ai/AiProvider.h ai/AiProvider.cpp
            ai/ChatGptProvider.h ai/ChatGptProvider.cpp ai/OpenRouterProvider.h ai/OpenRouterProvider.cpp
            ai/AiStorage.h ai/SystemAiStorage.cpp ai/OAuthLoopback.h ai/OAuthLoopback.cpp
            ai/Jwt.h ai/Jwt.cpp ai/SseParser.h ai/SseParser.cpp
)

target_link_libraries(rune PRIVATE Qt6::Gui Qt6::Quick Qt6::Concurrent Qt6::Network Qt6::Test QuaZip::QuaZip
                                   OpenSSL::Crypto PkgConfig::LIBSECRET)
target_include_directories(rune PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ai)

# --auto-test opens samples/test.docx from the repo.
target_compile_definitions(rune PRIVATE RUNE_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../samples")
