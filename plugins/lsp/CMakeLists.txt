# The LSP capability seam: a provider registry keyed by file extension and four
# normalized read-only navigation operations. Providers register capabilities;
# tool-lsp owns the model-facing surface.
araya_add_plugin(lsp
    SOURCES src/lsp.cpp
    DEPS araya::araya
    TEST tests/lsp_test.cpp
)
