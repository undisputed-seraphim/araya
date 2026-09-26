# The model-facing job controls over the jobs registry: job_output/job_list/
# job_kill plus completion-notice delivery. araya::jobs supplies the registry,
# araya::llm-bridge the user-message projection the notice uses, and
# araya::tools the tool registry (with the system-prompt registry).
araya_add_plugin(tool-jobs
    SOURCES src/tool_jobs.cpp
    DEPS araya::jobs araya::llm-bridge araya::tools
    TEST tests/tool_jobs_test.cpp
)
