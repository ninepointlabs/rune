cmake_minimum_required(VERSION 3.21)
project(rune VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The current, actively-developed implementation: a single-process app built
# on QML TextEdit + QTextDocument. See native/CMakeLists.txt and the
# architecture notes in README.md for why this replaced the original
# LibreOfficeKit-based two-process design.
add_subdirectory(native)

# legacy-lok/ (the retired LibreOfficeKit engine/app pair, kept for
# reference) is NOT built by default. It still builds and its own test
# suite still passes if you want it: see legacy-lok/README.md.
