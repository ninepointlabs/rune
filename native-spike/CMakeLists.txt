# native-spike: validates QML TextEdit + QTextDocument as a LOK-free editing
# core. Standalone; not wired into the root build.
#
#   cmake -S native-spike -B native-spike/build -G Ninja
#   cmake --build native-spike/build

cmake_minimum_required(VERSION 3.21)
project(rune_native_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Quick)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(rune_native_spike main.cpp)
qt_add_qml_module(rune_native_spike
    URI RuneNativeSpike
    VERSION 1.0
    QML_FILES Main.qml
    SOURCES OdfExporter.h OdfExporter.cpp
)

target_link_libraries(rune_native_spike PRIVATE Qt6::Gui Qt6::Quick)
