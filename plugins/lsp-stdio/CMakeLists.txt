# The stdio language-server provider for the `lsp` seam: configured server
# commands and extension mappings, one pooled process per workspace, transient
# didOpen/query/didClose lifecycles. Boost.Process provides the child pipes.
araya_add_plugin(lsp-stdio
    SOURCES
        src/config.cpp
        src/connection.cpp
        src/instance.cpp
        src/lsp_stdio.cpp
        src/provider.cpp
        src/translate.cpp
    DEPS araya::araya araya::lsp araya::logger ${ARAYA_BOOST_PROCESS_LIBS}
    TEST tests/lsp_stdio_test.cpp
    TEST_INCLUDES src
)
