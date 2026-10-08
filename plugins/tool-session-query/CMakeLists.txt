# The model-facing session query/search tools and their policy section.
# araya::session-query supplies the search service, araya::session the store,
# araya::agent-loop the open-turn boundary, and araya::tools /
# araya::system-prompt the registries.
araya_add_plugin(tool-session-query
    SOURCES src/tool_session_query.cpp
    DEPS araya::session-query araya::tools araya::system-prompt araya::agent-loop
    TEST tests/tool_session_query_test.cpp
    TEST_DEPS araya::llm araya::llm-mock
)
