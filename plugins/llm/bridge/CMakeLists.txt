# The session <-> llm bridge: the header-only contract boundary between
# the session plugin's event JSON and the llm seam's typed messages. An
# INTERFACE target (no object code): consumers link it to gain the
# session and llm include paths together. Adapters that link araya::llm
# alone never see session types.
add_library(araya_llm_bridge INTERFACE)
add_library(araya::llm-bridge ALIAS araya_llm_bridge)

target_include_directories(araya_llm_bridge INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_llm_bridge INTERFACE
    araya::session
    araya::llm
)

if(ARAYA_TESTING)
    # The bridge test references no object code from the static libs it
    # links; GNU ld's one-pass archive extraction then never pulls the
    # boost::json member that defines Boost's error-category holders,
    # leaving the engine's debug relocations unresolved. An unclosed
    # group extends over the whole link line and makes extraction
    # converge (the same recipe the TSan build needs).
    araya_add_test(araya_llm_bridge_tests
        SOURCES tests/bridge_test.cpp
        DEPS araya::llm-bridge
        LINK_OPTIONS "LINKER:--start-group"
    )
endif()
