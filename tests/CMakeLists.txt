add_library(medulla_test_module SHARED test_module.cpp)
target_include_directories(medulla_test_module PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_executable(medulla_tests
    context_test.cpp
    effects_test.cpp
    events_test.cpp
    manager_test.cpp
    module_loader_test.cpp
    module_test.cpp
    reconcile_test.cpp
    task_test.cpp
    version_test.cpp
)

target_link_libraries(medulla_tests PRIVATE
    medulla::medulla
    Catch2::Catch2WithMain
    ${CMAKE_DL_LIBS}
)

add_dependencies(medulla_tests medulla_test_module)

target_compile_definitions(medulla_tests PRIVATE
    TEST_MODULE_PATH="$<TARGET_FILE:medulla_test_module>"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medulla_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(Catch)
catch_discover_tests(medulla_tests)
