# The llm seam. OpenSSL is linked PUBLIC so adapters including
# araya/llm/http.hpp get TLS support without extra wiring; the OS
# provides the shared libraries (dynamic linking, revisit later).
find_package(OpenSSL REQUIRED)

# boost::json::value is a header type with a compiled implementation;
# module-mode search so a header-only Boost elsewhere cannot shadow it
# (same recipe as the session plugin).
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.83 REQUIRED COMPONENTS json)

araya_add_plugin(llm
    SOURCES src/http.cpp src/llm.cpp src/plugin.cpp
    DEPS araya::araya Boost::json OpenSSL::SSL OpenSSL::Crypto
    TEST tests/llm_test.cpp
)
