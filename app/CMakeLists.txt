# rune: Qt Quick frontend. Talks to rune-engine over a Unix socket; no LOK here.

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Quick Network)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(rune main.cpp)
qt_add_qml_module(rune
    URI Rune
    VERSION 1.0
    QML_FILES Main.qml FileBar.qml Toolbar.qml ContextualBar.qml DocumentCanvas.qml StatusBar.qml
              ToolButton.qml ToolSeparator.qml
    SOURCES BridgeSocket.h BridgeSocket.cpp ClipboardHelper.h ClipboardHelper.cpp
)

target_compile_definitions(rune PRIVATE
    RUNE_DEFAULT_DOC="${CMAKE_CURRENT_SOURCE_DIR}/../samples/test.docx")
target_link_libraries(rune PRIVATE Qt6::Gui Qt6::Quick Qt6::Network)
