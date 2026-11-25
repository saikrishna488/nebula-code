add_library(nebula_services STATIC
    src/lsp/LspClient.cpp
    src/lsp/LspClient.h
    src/lsp/LspPanel.cpp
    src/lsp/LspPanel.h
    src/git/GitPanel.cpp
    src/git/GitPanel.h
    src/git/GitRepository.cpp
    src/git/GitRepository.h
)
set_target_properties(nebula_services PROPERTIES AUTOMOC ON)
target_compile_features(nebula_services PUBLIC cxx_std_20)
target_include_directories(nebula_services PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(nebula_services PUBLIC Qt6::Widgets)

if(BUILD_TESTING)
    add_subdirectory(tests/services)
endif()
