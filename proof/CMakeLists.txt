# The refinement bridge: a standalone paper-rule oracle (no araya
# dependencies), conformance tests driving the engine and the oracle with
# identical sequences, and the opt-in TLA+ model check (see tla/).

add_library(araya_oracle INTERFACE)
target_include_directories(araya_oracle INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/oracle>
)

if(ARAYA_TESTING)
    # The exhaustive suites spin up one runtime per sequence, so the
    # discovered tests get a generous timeout.
    araya_add_test(araya_conformance_tests
        SOURCES conformance/conformance_test.cpp
        DEPS araya::araya araya_oracle
        DISCOVERY_PROPERTIES TIMEOUT 1800
    )
endif()

# The TLA+ model check is always wired when the tools are available: it
# takes ~seconds and guards the paper refinement on every ctest run. If
# Java and the TLA+ tools are not installed, it is skipped with a STATUS
# message - nothing else in the build should depend on it.
#
# Coq (tier 3, proof/coq/) is parked and deliberately never wired into
# the build.
unset(TLA2TOOLS_JAR CACHE)
if(DEFINED ENV{TLA2TOOLS} AND NOT "$ENV{TLA2TOOLS}" STREQUAL "")
    set(TLA2TOOLS_JAR "$ENV{TLA2TOOLS}")
endif()
find_program(ARAYA_JAVA java)
if(NOT TLA2TOOLS_JAR)
    find_file(TLA2TOOLS_JAR tla2tools.jar
        PATHS /usr/local/share/tla+ /usr/share/java /usr/share/tla+
        NO_DEFAULT_PATH)
endif()
if(ARAYA_JAVA AND TLA2TOOLS_JAR)
    add_test(NAME tlc_refinement
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tla/run-tlc.sh
                "${TLA2TOOLS_JAR}")
    set_tests_properties(tlc_refinement PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tla
        TIMEOUT 900)
else()
    message(STATUS
        "TLA+ tools not found (need java and tla2tools.jar): skipping "
        "the tlc_refinement model check")
endif()
