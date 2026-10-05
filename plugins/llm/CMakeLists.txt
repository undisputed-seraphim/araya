# The llm seam. OpenSSL is linked PUBLIC so adapters including
# araya/llm/http.hpp get TLS support without extra wiring; the OS
# provides the shared libraries (dynamic linking, revisit later). A parent
# that already found OpenSSL keeps its targets.
if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

araya_add_plugin(llm
    SOURCES src/http.cpp src/llm.cpp src/plugin.cpp
    DEPS araya::araya OpenSSL::SSL OpenSSL::Crypto
    TEST tests/llm_test.cpp
)
