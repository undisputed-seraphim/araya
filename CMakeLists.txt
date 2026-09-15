cmake_minimum_required(VERSION 3.24)

project(medulla VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Boost 1.83 REQUIRED)
find_package(Threads REQUIRED)
find_package(Catch2 3 QUIET)

option(MEDULLA_ENFORCE_INVARIANTS
    "Check engine invariants even in release builds (violations throw)"
    OFF)

add_library(medulla STATIC
    src/abi_host.cpp
    src/activation.cpp
    src/context.cpp
    src/effects.cpp
    src/events.cpp
    src/fiber.cpp
    src/module_loader.cpp
    src/runtime.cpp
    src/version.cpp
)
add_library(medulla::medulla ALIAS medulla)

set_target_properties(medulla PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME medulla
)

target_include_directories(medulla PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(medulla PUBLIC Boost::boost Threads::Threads)

if(MEDULLA_ENFORCE_INVARIANTS)
    target_compile_definitions(medulla
        PUBLIC MEDULLA_ENFORCE_INVARIANTS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medulla PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS medulla
    EXPORT medullaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT medullaTargets
    NAMESPACE medulla::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/medulla
)

configure_package_config_file(
    cmake/medullaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/medullaConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/medulla
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/medullaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/medullaConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/medullaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/medulla
)
