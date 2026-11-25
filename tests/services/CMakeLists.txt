find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nebula_lsp_fixture fake_lsp.cpp)
target_link_libraries(nebula_lsp_fixture PRIVATE Qt6::Core)
target_compile_features(nebula_lsp_fixture PRIVATE cxx_std_20)

add_executable(nebula_service_tests service_tests.cpp)
set_target_properties(nebula_service_tests PROPERTIES AUTOMOC ON)
target_link_libraries(nebula_service_tests PRIVATE nebula_services Qt6::Test)
target_compile_definitions(nebula_service_tests PRIVATE
    NEBULA_LSP_FIXTURE="$<TARGET_FILE:nebula_lsp_fixture>"
    NEBULA_CLANGD_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/clangd_fixture")
add_dependencies(nebula_service_tests nebula_lsp_fixture)
add_test(NAME nebula_service_tests COMMAND nebula_service_tests)
