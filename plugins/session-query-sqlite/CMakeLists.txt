# The FTS5-backed session query engine. araya::session-query supplies the
# service interface, extraction, filters, and the non-search reads; araya::sqlite3
# is the vendored amalgamation (thirdparty/sqlite.cmake) with FTS5 enabled.
araya_add_plugin(session-query-sqlite
    SOURCES src/sqlite.cpp
    DEPS araya::session araya::persistence araya::session-query araya::sqlite3
    TEST tests/sqlite_test.cpp
)
