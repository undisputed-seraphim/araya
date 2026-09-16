# The json component needs the compiled Boost.JSON library; prefer the
# module-mode search so a header-only Boost elsewhere (e.g. a user-local
# install without libboost_json) cannot shadow it.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.83 REQUIRED COMPONENTS json)

add_library(medulla_session STATIC
    src/plugin.cpp
    src/repair.cpp
    src/store.cpp
    src/surface.cpp
)
add_library(medulla::session ALIAS medulla_session)

set_target_properties(medulla_session PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(medulla_session PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(medulla_session PUBLIC
    medulla::medulla
    Boost::json
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medulla_session PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(medulla_session_tests tests/session_test.cpp)
    target_link_libraries(medulla_session_tests PRIVATE
        medulla::session
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(medulla_session_tests)
endif()
