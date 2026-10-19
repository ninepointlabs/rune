# Built by packaging/appimage/build.sh against the AppImage's Qt; see main.cpp.
cmake_minimum_required(VERSION 3.21)
project(tls_probe LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(tls-probe main.cpp)
target_link_libraries(tls-probe PRIVATE Qt6::Network)
# Finds the bundled Qt the way the rune binary does.
set_target_properties(tls-probe PROPERTIES BUILD_RPATH "$ORIGIN/../lib" SKIP_BUILD_RPATH OFF BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/../lib")
