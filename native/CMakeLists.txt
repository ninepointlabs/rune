# native: single-process Rune built on QML TextEdit + QTextDocument, replacing
# the LibreOfficeKit engine/app pair once feature-complete. Standalone; not
# wired into the root build yet.
#
#   cmake -S native -B native/build -G Ninja
#   cmake --build native/build

cmake_minimum_required(VERSION 3.21)
project(rune_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 6.7: QQuickTextDocument::setTextDocument(), which DocumentController relies on.
find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Quick)
# Unzipping .odt files for OdfReader.
find_package(QuaZip-Qt6 REQUIRED)

qt_standard_project_setup(REQUIRES 6.7)

qt_add_executable(rune_native main.cpp AutoTest.h AutoTest.cpp)
qt_add_qml_module(rune_native
    URI RuneNative
    VERSION 1.0
    QML_FILES Main.qml ToolButton.qml
    SOURCES DocumentController.h DocumentController.cpp OdfReader.h OdfReader.cpp
)

target_link_libraries(rune_native PRIVATE Qt6::Gui Qt6::Quick QuaZip::QuaZip)
