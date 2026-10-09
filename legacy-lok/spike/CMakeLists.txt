cmake_minimum_required(VERSION 3.21)
project(rune_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Quick)

# LibreOfficeKit has no pkg-config / CMake package. It is header-only on the
# client side: LibreOfficeKitInit.h dlopen()s libsofficeapp.so (or
# libmergedlo.so) from OFFICE_HOME/program at runtime, so we only need the
# include path and libdl. liblibreofficekitgtk.so is the GTK widget wrapper
# and is NOT needed (linking it would pull GTK into a Qt process).
set(LO_INCLUDE_DIR "/usr/include/LibreOfficeKit" CACHE PATH "LibreOfficeKit headers")
set(LO_PROGRAM_DIR "/usr/lib/libreoffice/program" CACHE PATH "LibreOffice program dir")
if(NOT EXISTS "${LO_INCLUDE_DIR}/LibreOfficeKit.hxx")
    message(FATAL_ERROR "LibreOfficeKit.hxx not found in ${LO_INCLUDE_DIR}")
endif()

qt_standard_project_setup()

qt_add_executable(rune_spike main.cpp)
qt_add_qml_module(rune_spike
    URI RuneSpike
    VERSION 1.0
    QML_FILES Main.qml
)

target_include_directories(rune_spike PRIVATE "${LO_INCLUDE_DIR}")
target_compile_definitions(rune_spike PRIVATE
    LOK_USE_UNSTABLE_API
    LO_PROGRAM_DIR="${LO_PROGRAM_DIR}"
    SPIKE_DEFAULT_DOC="${CMAKE_CURRENT_SOURCE_DIR}/test.docx")
target_link_libraries(rune_spike PRIVATE Qt6::Gui Qt6::Quick ${CMAKE_DL_LIBS})
