cmake_minimum_required(VERSION 3.24)

project(araya VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/Format.cmake)
include(cmake/Warnings.cmake)
include(cmake/Testing.cmake)
include(cmake/Plugin.cmake)

# -- build surface --------------------------------------------------------
# Every build-surface option defaults to ON only when araya is the top-level
# project. As a subproject (add_subdirectory) nothing is pulled in unless the
# consumer opts in, so a vendored araya does not drag in the app, the bench,
# the tests, the proof bridge, or install rules.

option(ARAYA_ENFORCE_INVARIANTS
    "Check engine invariants even in release builds (violations throw)"
    OFF)

option(ARAYA_BUILD_PLUGINS
    "Build the first-party plugin libraries"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_BUILD_BENCH
    "Build the profiling bench application (requires plugins)"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_BUILD_APP
    "Build the unified araya application (console + TUI; requires plugins)"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_BUILD_TESTS
    "Build the Catch2 test suites (requires Catch2)"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_BUILD_PROOF
    "Build the proof/conformance bridge (and wire the TLA+ check)"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_INSTALL
    "Install the araya library, headers, and CMake package"
    ${PROJECT_IS_TOP_LEVEL})

option(ARAYA_WERROR
    "Promote warnings to errors on first-party targets"
    ${PROJECT_IS_TOP_LEVEL})

# The first-party plugins and support libraries to build. Top-level builds
# build all of them; a consumer passes a dependency-closed subset, e.g.
#   -DARAYA_PLUGINS="logger;timer;llm;llm-bridge;session;persistence;system-prompt;tools;agent-loop"
# The bridge lives at llm/bridge; its friendly list name is llm-bridge.
set(ARAYA_PLUGINS
    agent-instructions agent-loop attachment coreutil fs fs-observation-policy
    goal goal-round-driver jobs llm llm-bridge llm-mock llm-openai logger
    persistence sandbox sandbox-policy session shell skill subagent-fork
    subagents system-prompt timer tool-ask-user tool-goal tool-jobs
    tool-read-image tool-skill tool-subagent tool-subagent-control tool-todo
    tool-workflow tool-web tools user-questions web workflow
    CACHE STRING "First-party plugins and support libraries to build")

# Landlock detection is only meaningful when the sandbox plugin is selected.
include(cmake/Landlock.cmake)

# -- dependencies ---------------------------------------------------------
# Boost is discovered once. Boost.JSON is a compiled component and the
# engine's public util/json.hpp depends on it, so it rides the engine's
# PUBLIC link interface. Boost.Process v2 and Boost.Filesystem are used only
# by the shell and workflow plugins, so they are required only when one of
# those is in the selected set.
#
# A parent that already found Boost keeps its own targets; otherwise araya
# discovers module-mode Boost (Boost_NO_BOOST_CMAKE keeps a header-only Boost
# elsewhere from shadowing the compiled components).
set(ARAYA_BOOST_MIN 1.91)

set(_araya_need_process OFF)
if(ARAYA_BUILD_PLUGINS)
    foreach(_plugin IN LISTS ARAYA_PLUGINS)
        if(_plugin STREQUAL "shell" OR _plugin STREQUAL "workflow")
            set(_araya_need_process ON)
        endif()
    endforeach()
endif()

if(NOT TARGET Boost::json)
    set(Boost_NO_BOOST_CMAKE ON)
    find_package(Boost ${ARAYA_BOOST_MIN} REQUIRED COMPONENTS json)
endif()

if(_araya_need_process AND NOT TARGET Boost::process)
    set(Boost_NO_BOOST_CMAKE ON)
    find_package(Boost ${ARAYA_BOOST_MIN} REQUIRED COMPONENTS process filesystem)
endif()

# The header-only target differs between Boost's module (Boost::boost) and
# config (Boost::headers) packages; normalize so either parent discovery works.
if(TARGET Boost::boost)
    set(ARAYA_BOOST_HEADERS Boost::boost)
else()
    set(ARAYA_BOOST_HEADERS Boost::headers)
endif()

# The process-spawning plugins (shell, workflow) share this link set; keep the
# component list here rather than repeating it in each plugin's CMakeLists.
set(ARAYA_BOOST_PROCESS_LIBS Boost::process Boost::filesystem)

find_package(Threads REQUIRED)

set(ARAYA_TESTING OFF)
if(ARAYA_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        set(ARAYA_TESTING ON)
    endif()
endif()

add_library(araya STATIC
    src/abi_host.cpp
    src/activation.cpp
    src/context.cpp
    src/effects.cpp
    src/events.cpp
    src/fiber.cpp
    src/module_loader.cpp
    src/plugin_context.cpp
    src/runtime.cpp
    src/version.cpp
)
add_library(araya::araya ALIAS araya)

set_target_properties(araya PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME araya
)

target_include_directories(araya PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(araya PUBLIC ${ARAYA_BOOST_HEADERS} Boost::json Threads::Threads)

if(ARAYA_WERROR)
    araya_enable_warnings(araya WERROR)
else()
    araya_enable_warnings(araya)
endif()

if(ARAYA_ENFORCE_INVARIANTS)
    target_compile_definitions(araya
        PUBLIC ARAYA_ENFORCE_INVARIANTS)
endif()

if(ARAYA_TESTING)
    # Shared test-support headers (the runtime harness, the llm chunk
    # builders); an INTERFACE target so plugin tests reach them through
    # one dependency.
    add_library(araya_test_support INTERFACE)
    add_library(araya::test_support ALIAS araya_test_support)
    target_include_directories(araya_test_support INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(araya_test_support INTERFACE Catch2::Catch2WithMain)

    enable_testing()
    add_subdirectory(tests)
endif()

if(ARAYA_BUILD_PLUGINS)
    include(thirdparty/quill.cmake)
    add_subdirectory(plugins)
endif()

if(ARAYA_BUILD_BENCH)
    if(ARAYA_BUILD_PLUGINS)
        add_subdirectory(bench)
    else()
        message(WARNING "ARAYA_BUILD_BENCH=ON requires ARAYA_BUILD_PLUGINS=ON; skipping the bench")
    endif()
endif()

if(ARAYA_BUILD_APP)
    if(ARAYA_BUILD_PLUGINS)
        include(thirdparty/ftxui.cmake)
        add_subdirectory(apps)
    else()
        message(WARNING "ARAYA_BUILD_APP=ON requires ARAYA_BUILD_PLUGINS=ON; skipping the app")
    endif()
endif()

if(ARAYA_BUILD_PROOF)
    add_subdirectory(proof)
endif()

# -- install / export -----------------------------------------------------
# Only emit install rules when araya owns the install prefix. The exported
# araya::araya target (and the installed CMake package) is what downstream
# find_package consumers link.
if(ARAYA_INSTALL)
    install(TARGETS araya
        EXPORT arayaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT arayaTargets
        NAMESPACE araya::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/araya
    )

    configure_package_config_file(
        cmake/arayaConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/arayaConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/araya
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/arayaConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/arayaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/arayaConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/araya
    )
endif()
