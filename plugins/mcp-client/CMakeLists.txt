# The MCP client bridge: connect stdio Model Context Protocol servers,
# discover their tools, and register them as `mcp__<server>__<tool>`. Provides
# the `mcp` service (consumed by mcp-resources, M13). Hand-rolled JSON-RPC 2.0
# (no MCP SDK); stdio transport over Boost.Process v2; OpenSSL supplies the
# public-name hash suffix.
find_package(OpenSSL REQUIRED)

araya_add_plugin(mcp-client
    SOURCES
        src/config.cpp
        src/connection.cpp
        src/mcp_client.cpp
        src/naming.cpp
        src/stdio_transport.cpp
    DEPS araya::araya araya::tools araya::system-prompt
         OpenSSL::Crypto ${ARAYA_BOOST_PROCESS_LIBS}
    TEST tests/mcp_test.cpp
    TEST_INCLUDES src
)
