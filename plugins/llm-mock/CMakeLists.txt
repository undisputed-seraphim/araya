araya_add_plugin(llm-mock
    SOURCES src/mock.cpp src/plugin.cpp
    DEPS araya::llm
    TEST tests/mock_test.cpp
)
