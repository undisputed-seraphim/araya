# The refinement bridge: a standalone paper-rule oracle (no araya
# dependencies), conformance tests driving the engine and the oracle with
# identical sequences, and the opt-in TLA+ model check (see tla/).

add_library(araya_oracle INTERFACE)
target_include_directories(araya_oracle INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/oracle>
)

if(Catch2_FOUND)
    add_executable(araya_conformance_tests conformance/conformance_test.cpp)
    target_link_libraries(araya_conformance_tests PRIVATE
        araya::araya
        araya_oracle
        Catch2::Catch2WithMain
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(araya_conformance_tests
            PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    include(Catch)
    # The exhaustive suites spin up one runtime per sequence; give the
    # discovered tests generous timeouts.
    catch_discover_tests(araya_conformance_tests PROPERTIES TIMEOUT 1800)
endif()

# The TLA+ model check is opt-in: it needs Java and the TLA+ tools
# (tla2tools.jar), which downstream users may not want to install.
if(ARAYA_ENABLE_PROOF)
    find_program(ARAYA_JAVA java REQUIRED)
    unset(TLA2TOOLS_JAR CACHE)
    if(DEFINED ENV{TLA2TOOLS} AND NOT "$ENV{TLA2TOOLS}" STREQUAL "")
        set(TLA2TOOLS_JAR "$ENV{TLA2TOOLS}")
    endif()
    if(NOT TLA2TOOLS_JAR)
        find_file(TLA2TOOLS_JAR tla2tools.jar
            PATHS /usr/local/share/tla+ /usr/share/java /usr/share/tla+
            NO_DEFAULT_PATH)
    endif()
    if(NOT TLA2TOOLS_JAR)
        message(FATAL_ERROR
            "ARAYA_ENABLE_PROOF: TLA+ tools not found; install them or "
            "set TLA2TOOLS=/path/to/tla2tools.jar")
    endif()
    add_test(NAME tlc_refinement
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tla/run-tlc.sh
                "${TLA2TOOLS_JAR}")
    set_tests_properties(tlc_refinement PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tla
        TIMEOUT 900)
endif()
