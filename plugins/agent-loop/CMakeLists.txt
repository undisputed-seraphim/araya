# The agent loop: turns and steps over the llm seam with
# plugin-registered tools. Depends on the session store for durable
# history and the llm service for streaming; Boost.JSON comes
# transitively through both.
araya_add_plugin(agent-loop
    SOURCES src/agent.cpp src/bridge.cpp src/plugin.cpp
    DEPS araya::session araya::llm
    TEST tests/agent_test.cpp
)
