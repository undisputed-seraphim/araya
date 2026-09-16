add_library(medulla_logger STATIC
    src/logger.cpp
    src/plugin.cpp
)
add_library(medulla::logger ALIAS medulla_logger)

set_target_properties(medulla_logger PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(medulla_logger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# quill is a forced dependency of the logger plugin: PUBLIC so consumers
# of medulla::logger link it transitively.
target_link_libraries(medulla_logger PUBLIC
    medulla::medulla
    quill::quill
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medulla_logger PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(medulla_logger_tests tests/logger_test.cpp)
    target_link_libraries(medulla_logger_tests PRIVATE
        medulla::logger
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(medulla_logger_tests)
endif()
