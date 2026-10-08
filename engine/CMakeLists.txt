# rune-engine: headless LibreOfficeKit daemon. Pure C++ (no Qt).

find_package(PNG REQUIRED)

# LibreOfficeKit is header-only on the client side: LibreOfficeKitInit.h
# dlopen()s libsofficeapp.so / libmergedlo.so from LO_PROGRAM_DIR at runtime,
# so we only need the headers and libdl (same pattern as spike/).
set(LO_INCLUDE_DIR "/usr/include/LibreOfficeKit" CACHE PATH "LibreOfficeKit headers")
set(LO_PROGRAM_DIR "/usr/lib/libreoffice/program" CACHE PATH "LibreOffice program dir")
if(NOT EXISTS "${LO_INCLUDE_DIR}/LibreOfficeKit.hxx")
    message(FATAL_ERROR "LibreOfficeKit.hxx not found in ${LO_INCLUDE_DIR}")
endif()

add_executable(rune-engine engine.cpp ai_manager.cpp md_to_html.cpp)
target_compile_features(rune-engine PRIVATE cxx_std_17)
target_compile_options(rune-engine PRIVATE -Wall -Wextra)
target_include_directories(rune-engine PRIVATE "${LO_INCLUDE_DIR}")
target_compile_definitions(rune-engine PRIVATE
    LOK_USE_UNSTABLE_API
    LO_PROGRAM_DIR="${LO_PROGRAM_DIR}")
target_link_libraries(rune-engine PRIVATE PNG::PNG ${CMAKE_DL_LIBS})
