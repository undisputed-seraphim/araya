add_library(araya_timer STATIC
    src/plugin.cpp
    src/timer.cpp
)
add_library(araya::timer ALIAS araya_timer)

set_target_properties(araya_timer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_timer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_timer PUBLIC
    araya::araya
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_timer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_timer_tests tests/timer_test.cpp)
    target_link_libraries(araya_timer_tests PRIVATE
        araya::timer
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(araya_timer_tests)
endif()
