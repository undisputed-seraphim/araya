# The deterministic mock adapter for tests and demos: canned chunks and
# injectable failures, zero network. Depends on the llm seam only.
araya_add_plugin(llm-mock
    SOURCES src/mock.cpp src/plugin.cpp
    DEPS araya::llm
    TEST tests/mock_test.cpp
)
