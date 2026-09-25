# The subagent delegation seam: the provider registry plus the in-process
# spawn provider and continuable-child orchestration. It drives child turns
# through the agent loop and stores children in the session store; the
# llm bridge supplies the final-output fold and settlement message.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.91 REQUIRED COMPONENTS json)

araya_add_plugin(subagents
    SOURCES src/subagents.cpp src/plugin.cpp
    DEPS araya::agent-loop araya::llm-bridge Boost::json
    TEST tests/subagents_test.cpp
    TEST_DEPS araya::session araya::llm araya::llm-mock araya::system-prompt araya::tools araya::agent-loop
)
