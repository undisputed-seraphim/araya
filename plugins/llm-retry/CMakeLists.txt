# The model-request retry executor: an `agent/request-error` waterfall listener
# that re-runs a failed step per the route's provider-owned retry policy.
araya_add_plugin(llm-retry
    SOURCES src/llm_retry.cpp
    DEPS araya::araya araya::llm araya::agent-loop
    TEST tests/llm_retry_test.cpp
)
