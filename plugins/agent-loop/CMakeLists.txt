# The agent loop: turns and steps over the llm seam, assembling the system
# prompt from the system-prompt registry and executing tools through the
# tools registry. araya::llm-bridge supplies the session store and the llm
# seam; araya::tools supplies the tool registry and (transitively) the
# system-prompt registry.
araya_add_plugin(agent-loop
    SOURCES src/agent.cpp src/plugin.cpp
    DEPS araya::llm-bridge araya::tools
    TEST tests/agent_test.cpp
)
