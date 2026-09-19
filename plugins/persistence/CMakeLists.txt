add_library(araya_persistence STATIC
    src/jsonl.cpp
    src/plugin.cpp
)
add_library(araya::persistence ALIAS araya_persistence)

set_target_properties(araya_persistence PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_persistence PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_persistence PUBLIC
    araya::araya
    # The public header exposes session types (session_header, the event
    # vocabulary): the format dependency, not the store - but the format
    # headers ship with the session plugin, so the link provides them.
    araya::session
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_persistence PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_persistence_tests tests/persistence_test.cpp)
    target_link_libraries(araya_persistence_tests PRIVATE
        araya::persistence
        araya::session
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(araya_persistence_tests)
endif()
