add_library(araya_logger STATIC
    src/logger.cpp
    src/plugin.cpp
)
add_library(araya::logger ALIAS araya_logger)

set_target_properties(araya_logger PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_logger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# quill is a forced dependency of the logger plugin: PUBLIC so consumers
# of araya::logger link it transitively.
target_link_libraries(araya_logger PUBLIC
    araya::araya
    quill::quill
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_logger PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_logger_tests tests/logger_test.cpp)
    target_link_libraries(araya_logger_tests PRIVATE
        araya::logger
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(araya_logger_tests)
endif()
