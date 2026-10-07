# The shared MCP resource tools and server-name prompt section over the `mcp`
# service from mcp-client. No configuration.
araya_add_plugin(mcp-resources
    SOURCES src/mcp_resources.cpp
    DEPS araya::araya araya::tools araya::system-prompt araya::mcp-client
    TEST tests/mcp_resources_test.cpp
)
