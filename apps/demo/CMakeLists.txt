# The demo components the console and the TUI share: the console plugin,
# the beacon/watcher availability pair, and the failure-injection bombs.
# App-layer, not a shipped plugin - it exists to make the engine's
# semantics visible in both surfaces.
add_library(araya_demo STATIC
    src/demo_plugins.cpp
)
add_library(araya::demo ALIAS araya_demo)

set_target_properties(araya_demo PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_demo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(araya_demo PUBLIC
    araya::araya
    araya::logger
    araya::session
    araya::timer
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
