include(FetchContent)
include(GenerateExportHeader)
include(CheckSymbolExists)

set(NEBULA_QTERMWIDGET_URL
    "https://github.com/lxqt/qtermwidget/releases/download/2.4.0/qtermwidget-2.4.0.tar.xz"
    CACHE STRING "URL or local path to the QTermWidget 2.4.0 source archive")
FetchContent_Declare(
    qtermwidget
    URL "${NEBULA_QTERMWIDGET_URL}"
    URL_HASH SHA256=ceb7d1991ecc0e75b751b6b7786c4962598d367c6e9a0b55f78fc12a49f25304
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR nebula-unused
)
FetchContent_MakeAvailable(qtermwidget)

# Build the widget directly to keep LXQt desktop build tools out of Nebula's dependencies.
set(qtermwidget_lib "${qtermwidget_SOURCE_DIR}/lib")
file(GLOB qtermwidget_sources CONFIGURE_DEPENDS "${qtermwidget_lib}/*.cpp")
add_library(qtermwidget6 SHARED ${qtermwidget_sources} "${qtermwidget_lib}/SearchBar.ui")
set_target_properties(qtermwidget6 PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    INSTALL_NAME_DIR "@rpath"
)
target_link_libraries(qtermwidget6 PUBLIC Qt6::Widgets)
target_include_directories(qtermwidget6 PUBLIC
    "${qtermwidget_lib}"
    "${CMAKE_CURRENT_BINARY_DIR}/qtermwidget-headers"
)
target_compile_definitions(qtermwidget6 PRIVATE
    HAVE_POSIX_OPENPT
    HAVE_SYS_TIME_H
    KB_LAYOUT_DIR=":/nebula/qtermwidget/kb-layouts"
    COLORSCHEMES_DIR=":/nebula/qtermwidget/color-schemes"
    TRANSLATIONS_DIR=":/nebula/qtermwidget/translations"
)
if(APPLE)
    target_compile_definitions(qtermwidget6 PRIVATE HAVE_UTMPX UTMPX_COMPAT)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(qtermwidget6 PRIVATE HAVE_PTSNAME HAVE_GRANTPT HAVE_UNLOCKPT)
endif()
check_symbol_exists(updwtmpx "utmpx.h" NEBULA_HAVE_UPDWTMPX)
if(NEBULA_HAVE_UPDWTMPX)
    target_compile_definitions(qtermwidget6 PRIVATE HAVE_UPDWTMPX)
endif()

set(QTERMWIDGET_VERSION_MAJOR 2)
set(QTERMWIDGET_VERSION_MINOR 4)
set(QTERMWIDGET_VERSION_PATCH 0)
configure_file("${qtermwidget_lib}/qtermwidget_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qtermwidget-headers/qtermwidget_version.h" @ONLY)
generate_export_header(qtermwidget6 BASE_NAME QTERMWIDGET
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/qtermwidget-headers/qtermwidget_export.h")
qt_add_resources(qtermwidget6 nebula_terminal_data
    PREFIX "/nebula/qtermwidget"
    BASE "${qtermwidget_lib}"
    FILES
        "${qtermwidget_lib}/kb-layouts/default.keytab"
        "${qtermwidget_lib}/color-schemes/BlackOnWhite.colorscheme"
        "${qtermwidget_lib}/color-schemes/WhiteOnBlack.colorscheme"
)

add_library(nebula_terminal STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../src/terminal/TerminalDock.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/terminal/TerminalSession.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/terminal/TerminalSession.h"
)
set_target_properties(nebula_terminal PROPERTIES AUTOMOC ON)
target_include_directories(nebula_terminal PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src")
target_link_libraries(nebula_terminal PUBLIC Qt6::Widgets PRIVATE qtermwidget6)

if(BUILD_TESTING)
    add_executable(nebula_terminal_tests
        "${CMAKE_CURRENT_LIST_DIR}/../tests/terminal/TerminalSessionTest.cpp")
    set_target_properties(nebula_terminal_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(nebula_terminal_tests PRIVATE nebula_terminal qtermwidget6 Qt6::Test)
    add_test(NAME terminal-session COMMAND nebula_terminal_tests -platform offscreen)
    set_tests_properties(terminal-session PROPERTIES TIMEOUT 30)
endif()
