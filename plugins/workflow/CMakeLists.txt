# The workflow engine seam: an out-of-process Node worker running a plain-JS
# orchestration script over a framed JSON control channel, driving child
# agents through the subagent seam.
araya_add_plugin(workflow
    SOURCES src/workflow.cpp src/process_worker.cpp src/protocol.cpp src/worker_source.cpp src/plugin.cpp
    DEPS araya::subagents ${ARAYA_BOOST_PROCESS_LIBS}
    TEST tests/workflow_test.cpp
    TEST_INCLUDES src
    TEST_DEPS araya::llm-mock
)
