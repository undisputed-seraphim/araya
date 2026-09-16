add_library(medulla_timer STATIC
    src/plugin.cpp
    src/timer.cpp
)
add_library(medulla::timer ALIAS medulla_timer)

set_target_properties(medulla_timer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(medulla_timer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(medulla_timer PUBLIC
    medulla::medulla
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medulla_timer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(medulla_timer_tests tests/timer_test.cpp)
    target_link_libraries(medulla_timer_tests PRIVATE
        medulla::timer
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(medulla_timer_tests)
endif()
