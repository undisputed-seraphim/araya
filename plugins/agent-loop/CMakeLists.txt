# The agent loop: turns and steps over the llm seam, assembling the system
# prompt from the system-prompt registry and executing tools through the
# tools registry. Depends on the session store for durable history and the
# llm service for streaming; the session <-> llm bridge supplies the
# envelope conversion.
araya_add_plugin(agent-loop
    SOURCES src/agent.cpp src/plugin.cpp
    DEPS araya::llm-bridge araya::system-prompt araya::tools
    TEST tests/agent_test.cpp
)
