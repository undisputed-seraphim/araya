# Workspace instruction discovery and rendering as an agent context producer.
araya_add_plugin(agent-instructions
    SOURCES src/agent_instructions.cpp
    DEPS araya::agent-loop
    TEST tests/agent_instructions_test.cpp
    TEST_DEPS araya::llm-mock
)
