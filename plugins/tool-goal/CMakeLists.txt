# The model-facing goal tools and their policy section. araya::goal supplies
# the goal domain, araya::agent-loop the driver boundary (open turn), and
# araya::tools/araya::system-prompt the registries.
araya_add_plugin(tool-goal
    SOURCES src/tool_goal.cpp
    DEPS araya::goal araya::tools araya::system-prompt araya::agent-loop
    TEST tests/tool_goal_test.cpp
    TEST_DEPS araya::llm araya::llm-mock
)
