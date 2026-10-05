# The first-party plugins and support libraries to build. The list comes from
# the root CMakeLists (ARAYA_PLUGINS); top-level builds default to all of
# them, and a consumer passes a dependency-closed subset. `llm-bridge` is the
# friendly name for the header-only bridge that lives at llm/bridge.
foreach(_plugin IN LISTS ARAYA_PLUGINS)
    if(_plugin STREQUAL "llm-bridge")
        add_subdirectory(llm/bridge)
    else()
        add_subdirectory(${_plugin})
    endif()
endforeach()
