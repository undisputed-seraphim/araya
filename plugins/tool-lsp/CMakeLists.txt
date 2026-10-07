# The model-facing `lsp` tool over the `lsp` seam: one read-only tool, four
# operations, one-based coordinates, result caps, and a per-call timeout.
araya_add_plugin(tool-lsp
    SOURCES src/tool_lsp.cpp
    DEPS araya::araya araya::lsp araya::tools araya::system-prompt araya::session
    TEST tests/tool_lsp_test.cpp
)
