add_library(araya_test_module SHARED test_module.cpp)
target_include_directories(araya_test_module PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_executable(araya_tests
    availability_test.cpp
    child_test.cpp
    config_test.cpp
    context_test.cpp
    diagnostics_test.cpp
    effects_test.cpp
    events_test.cpp
    invariants_test.cpp
    introspection_test.cpp
    manager_test.cpp
    model_test.cpp
    module_loader_test.cpp
    module_test.cpp
    polish_test.cpp
    realms_test.cpp
    reconcile_test.cpp
    task_test.cpp
    threading_test.cpp
    typed_test.cpp
    version_test.cpp
)

target_link_libraries(araya_tests PRIVATE
    araya::araya
    Catch2::Catch2WithMain
    ${CMAKE_DL_LIBS}
)

add_dependencies(araya_tests araya_test_module)

target_compile_definitions(araya_tests PRIVATE
    TEST_MODULE_PATH="$<TARGET_FILE:araya_test_module>"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(Catch)
catch_discover_tests(araya_tests)
