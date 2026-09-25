# The model-facing subagent tools. araya::subagents brings the seam and,
# transitively, the agent loop; araya::tools supplies the tool registry.
# The tests drive the whole stack plus the mock adapter.
araya_add_plugin(tool-subagent
    SOURCES src/tool_subagent.cpp
    DEPS araya::tools araya::subagents
    TEST tests/tool_subagent_test.cpp
    TEST_DEPS araya::llm-mock
)
