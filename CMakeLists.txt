cmake_minimum_required(VERSION 3.21)
project(rune VERSION 0.8.1 LANGUAGES CXX)

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

# Installation and packages. The binary's install rule is in native/.
#
#   cmake --install build --prefix /usr
#   cd build && cpack -G DEB    # or -G RPM; see packaging/README.md
include(GNUInstallDirs)
install(FILES packaging/rune.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/rune.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rune)

set(CPACK_PACKAGE_NAME rune)
set(CPACK_PACKAGE_VENDOR "Nine Point Labs")
set(CPACK_PACKAGE_CONTACT "Nine Point Labs <tim@ninepointlabs.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A word processor that writes with you")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ninepointlabs/rune")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)

# Shared-library dependencies are found automatically (dpkg-shlibdeps /
# rpm autoreq); QML modules are loaded at run time, so they're listed by hand.
# LibreOffice is only needed to open and save .docx/.doc.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION editors)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "qml6-module-qtcore, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-dialogs, qml6-module-qtquick-window, qml6-module-qtquick-templates, qml6-module-qtqml-workerscript")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libreoffice-writer-nogui | libreoffice-writer, gnome-keyring")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE MIT)
set(CPACK_RPM_PACKAGE_GROUP Applications/Productivity)
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtdeclarative")
set(CPACK_RPM_PACKAGE_SUGGESTS "libreoffice-writer")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/apps /usr/share/licenses)

include(CPack)
