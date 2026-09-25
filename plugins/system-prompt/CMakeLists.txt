araya_add_plugin(system-prompt
    SOURCES src/system_prompt.cpp src/plugin.cpp
    DEPS araya::araya
    TEST tests/system_prompt_test.cpp
)
