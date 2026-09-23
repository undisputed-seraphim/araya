# The agent loop: turns and steps over the llm seam with
# plugin-registered tools. Depends on the session store for durable
# history and the llm service for streaming; the session <-> llm bridge
# supplies the envelope conversion.
araya_add_plugin(agent-loop
    SOURCES src/agent.cpp src/plugin.cpp
    DEPS araya::llm-bridge
    TEST tests/agent_test.cpp
)
