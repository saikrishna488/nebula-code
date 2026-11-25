include(FetchContent)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core5Compat)

set(NEBULA_SCINTILLA_URL "https://www.scintilla.org/scintilla569.tgz" CACHE STRING
    "URL or local path to the pinned Scintilla source archive")
set(NEBULA_LEXILLA_URL "https://www.scintilla.org/lexilla555.tgz" CACHE STRING
    "URL or local path to the pinned Lexilla source archive")
FetchContent_Declare(
    scintilla
    URL "${NEBULA_SCINTILLA_URL}"
    URL_HASH SHA256=1622546ce1d3804b589292f1adae98ad1d87fa8c43bb401edc098adae12dcacb
)
FetchContent_Declare(
    lexilla
    URL "${NEBULA_LEXILLA_URL}"
    URL_HASH SHA256=fa4093ad5c48acf49a6f3eca1b75143a5c824ef1a6f5b0ee59e8c3d6a970f229
)
FetchContent_MakeAvailable(scintilla lexilla)

set(scintilla_qt_dir "${scintilla_SOURCE_DIR}/qt/ScintillaEditBase")
file(GLOB scintilla_core_sources CONFIGURE_DEPENDS "${scintilla_SOURCE_DIR}/src/*.cxx")
add_library(nebula_scintilla STATIC
    ${scintilla_core_sources}
    "${scintilla_qt_dir}/PlatQt.cpp"
    "${scintilla_qt_dir}/ScintillaEditBase.cpp"
    "${scintilla_qt_dir}/ScintillaEditBase.h"
    "${scintilla_qt_dir}/ScintillaQt.cpp"
    "${scintilla_qt_dir}/ScintillaQt.h"
)
set_target_properties(nebula_scintilla PROPERTIES AUTOMOC ON)
target_include_directories(nebula_scintilla SYSTEM PUBLIC
    "${scintilla_SOURCE_DIR}/include"
    "${scintilla_SOURCE_DIR}/src"
    "${scintilla_qt_dir}"
)
target_compile_definitions(nebula_scintilla PRIVATE SCINTILLA_QT=1)
target_link_libraries(nebula_scintilla PUBLIC Qt6::Widgets PRIVATE Qt6::Core5Compat)

file(GLOB lexilla_sources CONFIGURE_DEPENDS
    "${lexilla_SOURCE_DIR}/lexlib/*.cxx"
    "${lexilla_SOURCE_DIR}/lexers/Lex*.cxx"
)
add_library(nebula_lexilla STATIC "${lexilla_SOURCE_DIR}/src/Lexilla.cxx" ${lexilla_sources})
set_target_properties(nebula_lexilla PROPERTIES AUTOMOC OFF)
target_include_directories(nebula_lexilla SYSTEM PUBLIC "${lexilla_SOURCE_DIR}/include")
target_include_directories(nebula_lexilla SYSTEM PRIVATE
    "${scintilla_SOURCE_DIR}/include"
    "${lexilla_SOURCE_DIR}/lexlib"
)
target_compile_definitions(nebula_lexilla PRIVATE LEXILLA_NO_EXPORT=1)

add_library(nebula_editor STATIC
    "${PROJECT_SOURCE_DIR}/src/editor/EditorView.cpp"
    "${PROJECT_SOURCE_DIR}/src/editor/EditorView.h"
)
set_target_properties(nebula_editor PROPERTIES AUTOMOC ON)
target_include_directories(nebula_editor PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(nebula_editor PUBLIC Qt6::Widgets PRIVATE nebula_scintilla nebula_lexilla)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(editor_test "${PROJECT_SOURCE_DIR}/tests/EditorViewTest.cpp")
    set_target_properties(editor_test PROPERTIES AUTOMOC ON)
    target_link_libraries(editor_test PRIVATE nebula_editor nebula_scintilla nebula_lexilla Qt6::Test)
    add_test(NAME editor_test COMMAND editor_test)
    set_tests_properties(editor_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
