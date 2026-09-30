# The fork subagent provider: a child seeded with the parent's completed-turn
# prefix. araya::subagents supplies the provider seam (the service owns turn
# orchestration), araya::session the store the seeded child is prepared in.
araya_add_plugin(subagent-fork
    SOURCES src/subagent_fork.cpp
    DEPS araya::subagents araya::session
    TEST tests/subagent_fork_test.cpp
    TEST_DEPS araya::llm-mock
)
