# The web access service: URL fetch plus a configurable HTTP-JSON search
# provider, both over the shared araya::llm::http Beast client (which links
# OpenSSL for https). Header-first; the one source implements the service.
araya_add_plugin(web
    SOURCES src/web.cpp
    DEPS araya::araya araya::llm
    TEST tests/web_test.cpp
)
