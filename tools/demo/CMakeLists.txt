# The demo director: a Qt generic plugin that drives the unmodified rune
# binary to take the site's screenshots and record the video. Not part of
# the app's build; see README.md and make-media.py.
#
#   cmake -S tools/demo -B tools/demo/build -G Ninja && cmake --build tools/demo/build

cmake_minimum_required(VERSION 3.21)
project(runedemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Quick Qml Test)

add_library(runedemo MODULE RuneDemo.cpp)
target_link_libraries(runedemo PRIVATE Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
# Qt looks for generic plugins in <QT_PLUGIN_PATH>/generic.
set_target_properties(runedemo PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/generic)
