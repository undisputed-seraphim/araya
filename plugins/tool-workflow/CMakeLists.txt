# The model-facing `workflow` tool over the workflow engine seam.
araya_add_plugin(tool-workflow
    SOURCES src/tool_workflow.cpp
    DEPS araya::workflow araya::tools araya::system-prompt
    TEST tests/tool_workflow_test.cpp
    TEST_DEPS araya::llm-mock
)
