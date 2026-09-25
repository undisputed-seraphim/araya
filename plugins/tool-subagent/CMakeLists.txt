araya_add_plugin(tool-subagent
    SOURCES src/tool_subagent.cpp
    DEPS araya::tools araya::subagents
    TEST tests/tool_subagent_test.cpp
    TEST_DEPS araya::session araya::llm araya::llm-mock araya::system-prompt araya::tools araya::agent-loop
)
