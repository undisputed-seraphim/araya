# The shared shape of a first-party plugin target: a STATIC library named
# araya_<stem> (underscored), an araya::<stem> alias, PIC, the plugin's
# public include directory, the project warning set, and (when testing is
# enabled) one ctest executable per TEST/TESTS source:
#
#   araya_add_plugin(llm-mock
#       SOURCES src/mock.cpp src/plugin.cpp
#       DEPS araya::llm
#       TEST tests/mock_test.cpp        # or TESTS t1.cpp t2.cpp
#       TEST_DEPS araya::llm-mock       # extras beyond the plugin's PUBLIC deps
#       TEST_INCLUDES src
#       TEST_LINK_OPTIONS LINKER:--start-group
#   )
#
# A plugin's PUBLIC DEPS propagate to its test targets, so TEST_DEPS lists
# only test-only dependencies. Plugin-specific find_package calls (OpenSSL)
# stay in the plugin's own CMakeLists; Boost is found once at the root.
#
# araya_add_library is the same shape for first-party static libraries that
# are not plugins (the app-layer demo components).
function(araya_add_plugin stem)
    cmake_parse_arguments(PLUGIN "" "TEST"
        "SOURCES;DEPS;TESTS;TEST_DEPS;TEST_INCLUDES;TEST_LINK_OPTIONS" ${ARGN})

    string(REPLACE "-" "_" target_stem "${stem}")
    set(target "araya_${target_stem}")

    add_library(${target} STATIC ${PLUGIN_SOURCES})
    add_library(araya::${stem} ALIAS ${target})

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(${target} PUBLIC ${PLUGIN_DEPS})
    araya_enable_warnings(${target} WERROR)

    if(ARAYA_TESTING)
        set(_tests ${PLUGIN_TEST} ${PLUGIN_TESTS})
        list(LENGTH _tests _test_count)
        foreach(_test IN LISTS _tests)
            get_filename_component(_base "${_test}" NAME_WE)
            string(REGEX REPLACE "_test$" "" _base "${_base}")
            if(_test_count EQUAL 1)
                set(_test_target "${target}_tests")
            else()
                set(_test_target "${target}_${_base}_tests")
            endif()
            araya_add_test(${_test_target}
                SOURCES "${_test}"
                DEPS araya::${stem} ${PLUGIN_TEST_DEPS}
                INCLUDES ${PLUGIN_TEST_INCLUDES}
                LINK_OPTIONS ${PLUGIN_TEST_LINK_OPTIONS})
        endforeach()
    endif()
endfunction()

function(araya_add_library stem)
    cmake_parse_arguments(LIB "" "" "SOURCES;DEPS;INCLUDES" ${ARGN})

    string(REPLACE "-" "_" target_stem "${stem}")
    set(target "araya_${target_stem}")

    add_library(${target} STATIC ${LIB_SOURCES})
    add_library(araya::${stem} ALIAS ${target})

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    if(LIB_INCLUDES)
        target_include_directories(${target} PUBLIC
            $<BUILD_INTERFACE:${LIB_INCLUDES}>)
    endif()
    target_link_libraries(${target} PUBLIC ${LIB_DEPS})
    araya_enable_warnings(${target} WERROR)
endfunction()
