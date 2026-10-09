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
find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Quick Concurrent Test)
# Unzipping .odt files for OdfReader. (DocxBridge needs `soffice` at run time
# only, found in PATH.)
find_package(QuaZip-Qt6 REQUIRED)

qt_standard_project_setup(REQUIRES 6.7)

qt_add_executable(rune main.cpp AutoTest.h AutoTest.cpp)
qt_add_qml_module(rune
    URI Rune
    VERSION 1.0
    QML_FILES Main.qml ToolButton.qml
    SOURCES DocumentController.h DocumentController.cpp OdfReader.h OdfReader.cpp
            DocxBridge.h DocxBridge.cpp
)

target_link_libraries(rune PRIVATE Qt6::Gui Qt6::Quick Qt6::Concurrent Qt6::Test QuaZip::QuaZip)

# --auto-test opens samples/test.docx from the repo.
target_compile_definitions(rune PRIVATE RUNE_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../samples")
