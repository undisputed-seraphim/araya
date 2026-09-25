# The subagent delegation seam: the provider registry plus the in-process
# spawn provider and continuable-child orchestration. araya::agent-loop
# supplies the agent service (and, transitively, the session store and the
# llm bridge whose final-output fold and settlement message it uses).
araya_add_plugin(subagents
    SOURCES src/subagents.cpp src/plugin.cpp
    DEPS araya::agent-loop
    TEST tests/subagents_test.cpp
    TEST_DEPS araya::llm-mock
)
