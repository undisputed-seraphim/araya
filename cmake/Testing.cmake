# The project's shared test-target shape.
#
#   araya_add_test(<name>
#       SOURCES <src...>
#       [DEPS <targets...>]
#       [INCLUDES <dirs...>]
#       [LINK_OPTIONS <opts...>]
#       [DISCOVERY_PROPERTIES <props...>])
#
# Adds one Catch2 executable that links the shared test support (which
# carries Catch2WithMain and the harness headers) plus DEPS, applies the
# base warning set (not fatal for tests), and registers the discovered
# tests. DISCOVERY_PROPERTIES forwards to catch_discover_tests (e.g. a
# TIMEOUT for the exhaustive suites).
function(araya_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;INCLUDES;LINK_OPTIONS;DISCOVERY_PROPERTIES" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} araya::test_support)

    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_LINK_OPTIONS)
        target_link_options(${name} PRIVATE ${ARG_LINK_OPTIONS})
    endif()

    araya_enable_warnings(${name})

    include(Catch)
    if(ARG_DISCOVERY_PROPERTIES)
        catch_discover_tests(${name} PROPERTIES ${ARG_DISCOVERY_PROPERTIES})
    else()
        catch_discover_tests(${name})
    endif()
endfunction()
