cmake_minimum_required(VERSION 3.24)

project(araya VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Boost 1.83 REQUIRED)
find_package(Threads REQUIRED)
find_package(Catch2 3 QUIET)

option(ARAYA_ENFORCE_INVARIANTS
    "Check engine invariants even in release builds (violations throw)"
    OFF)

option(ARAYA_BUILD_PLUGINS
    "Build the first-party plugin libraries"
    ON)

option(ARAYA_BUILD_BENCH
    "Build the profiling bench application (requires plugins)"
    ON)

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

target_link_libraries(araya PUBLIC Boost::boost Threads::Threads)

if(ARAYA_ENFORCE_INVARIANTS)
    target_compile_definitions(araya
        PUBLIC ARAYA_ENFORCE_INVARIANTS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya PRIVATE -Wall -Wextra -Wpedantic -Werror)
    # GCC 14 false positive: awaitable frames are allocated through
    # Boost.Asio's aligned_new (aligned_alloc) and released through the
    # matching awaitable_frame_base::operator delete, which the
    # mismatch detector cannot see across the inlining boundary at -O2.
    target_compile_options(araya PRIVATE -Wno-mismatched-new-delete)
endif()

if(Catch2_FOUND)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ARAYA_BUILD_PLUGINS)
    include(thirdparty/quill.cmake)
    add_subdirectory(plugins)
    if(ARAYA_BUILD_BENCH)
        add_subdirectory(bench)
    endif()
endif()

add_subdirectory(proof)

include(cmake/Format.cmake)

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
