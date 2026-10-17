# The session query/search service: resolves live + persisted sessions,
# extracts searchable text, filters, and answers literal full-text queries.
araya_add_plugin(session-query
    SOURCES src/query.cpp
    DEPS araya::session araya::persistence araya::logger
    TEST tests/session_query_test.cpp
)
