# The automatic goal-round driver over the agent driver and the goal service.
araya_add_plugin(goal-round-driver
    SOURCES src/goal_round_driver.cpp
    DEPS araya::agent-loop araya::goal
    TEST tests/goal_round_driver_test.cpp
    TEST_DEPS araya::llm araya::llm-mock
)
