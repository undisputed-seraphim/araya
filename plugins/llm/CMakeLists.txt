# The llm seam. OpenSSL is linked PUBLIC so adapters including
# araya/llm/http.hpp get TLS support without extra wiring; the OS
# provides the shared libraries (dynamic linking, revisit later).
find_package(OpenSSL REQUIRED)

# boost::json::value is a header type with a compiled implementation;
# module-mode search so a header-only Boost elsewhere cannot shadow it
# (same recipe as the session plugin).
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.83 REQUIRED COMPONENTS json)

add_library(araya_llm STATIC
    src/http.cpp
    src/llm.cpp
    src/plugin.cpp
)
add_library(araya::llm ALIAS araya_llm)

set_target_properties(araya_llm PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_llm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_llm PUBLIC
    araya::araya
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_llm PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_llm_tests tests/llm_test.cpp)
    target_link_libraries(araya_llm_tests PRIVATE
        araya::llm
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(araya_llm_tests)
endif()
