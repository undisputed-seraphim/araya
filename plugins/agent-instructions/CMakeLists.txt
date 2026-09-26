# Workspace instruction discovery and rendering into a system-prompt section.
araya_add_plugin(agent-instructions
    SOURCES src/agent_instructions.cpp
    DEPS araya::system-prompt
    TEST tests/agent_instructions_test.cpp
)
