# The same-session goal domain: the event-sourced `goal` service and its
# projection. araya::session supplies the store/projection.
araya_add_plugin(goal
    SOURCES src/goal.cpp src/plugin.cpp
    DEPS araya::session
    TEST tests/goal_test.cpp
    TEST_DEPS araya::llm-bridge
)
