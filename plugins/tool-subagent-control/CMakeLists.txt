# The global continuable-child control tools. Mounted once, apart from the
# provider-bound araya::tool-subagent instances, so the fixed names
# `send_message`, `interrupt_agent`, and `list_agents` never collide when
# several delegation tools are loaded. araya::subagents brings the seam (and,
# transitively, the agent loop); araya::tools supplies the registry.
araya_add_plugin(tool-subagent-control
    SOURCES src/tool_subagent_control.cpp
    DEPS araya::tools araya::subagents
    TEST tests/tool_subagent_control_test.cpp
    TEST_DEPS araya::llm-mock araya::tool-subagent
)
