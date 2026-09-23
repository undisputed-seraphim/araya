# The shared shape of a first-party plugin target: a STATIC library
# named araya_<stem> (underscored), an araya::<stem> alias, PIC, the
# plugin's public include directory, -Wall/-Wextra/-Wpedantic/-Werror,
# and (when Catch2 is found) one default ctest executable per plugin:
#
#   araya_add_plugin(llm-mock
#       SOURCES src/mock.cpp src/plugin.cpp
#       DEPS araya::llm
#       TEST tests/mock_test.cpp
#       TEST_DEPS araya::araya   # extra test-only dependencies
#   )
#
# Plugins with unusual requirements keep their own targets: find_package
# calls (Boost.JSON, OpenSSL) stay in the plugin's CMakeLists, and a
# second test executable is added by hand after the call.
function(araya_add_plugin stem)
    cmake_parse_arguments(PARSE_ARGV 1 PLUGIN "" "TEST" "SOURCES;DEPS;TEST_DEPS")

    string(REPLACE "-" "_" target_stem "${stem}")
    set(target "araya_${target_stem}")

    add_library(${target} STATIC ${PLUGIN_SOURCES})
    add_library(araya::${stem} ALIAS ${target})

    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )

    target_link_libraries(${target} PUBLIC ${PLUGIN_DEPS})

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    if(Catch2_FOUND AND PLUGIN_TEST)
        add_executable(${target}_tests ${PLUGIN_TEST})
        target_link_libraries(${target}_tests PRIVATE
            araya::${stem}
            ${PLUGIN_TEST_DEPS}
            Catch2::Catch2WithMain
        )
        include(Catch)
        catch_discover_tests(${target}_tests)
    endif()
endfunction()
