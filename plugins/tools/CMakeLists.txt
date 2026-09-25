araya_add_plugin(tools
    SOURCES src/tools.cpp src/plugin.cpp
    DEPS araya::system-prompt
    TEST tests/tools_test.cpp
)
