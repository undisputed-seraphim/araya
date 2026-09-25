# The llm seam. OpenSSL is linked PUBLIC so adapters including
# araya/llm/http.hpp get TLS support without extra wiring; the OS
# provides the shared libraries (dynamic linking, revisit later).
find_package(OpenSSL REQUIRED)

araya_add_plugin(llm
    SOURCES src/http.cpp src/llm.cpp src/plugin.cpp
    DEPS araya::araya OpenSSL::SSL OpenSSL::Crypto
    TEST tests/llm_test.cpp
)
