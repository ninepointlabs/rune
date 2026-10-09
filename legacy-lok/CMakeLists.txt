cmake_minimum_required(VERSION 3.21)
project(rune_legacy_lok VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This is the RETIRED LibreOfficeKit-based implementation, kept for
# reference only -- it is not Rune's current app. See ../README.md for
# why it was replaced by ../native/ (single-process, QTextDocument-based).
# Not built as part of the root project; build it standalone if you want it:
#
#   cmake -S legacy-lok -B legacy-lok/build -G Ninja
#   cmake --build legacy-lok/build
#   python3 legacy-lok/engine/smoke_test.py

add_subdirectory(engine)
add_subdirectory(app)