# The demo components the console and the TUI share: the console plugin,
# the beacon/watcher availability pair, and the failure-injection bombs.
# App-layer, not a shipped plugin - it exists to make the engine's
# semantics visible in both surfaces.
araya_add_library(demo
    SOURCES src/demo_plugins.cpp
    DEPS araya::araya araya::logger araya::session araya::timer
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)
