# The session log download: a ZIP of the canonical JSONL log, subagent
# descendants, and referenced images. zlib provides deflate + crc32.
find_package(ZLIB REQUIRED)

araya_add_plugin(session-log-export
    SOURCES src/export.cpp
    DEPS araya::session araya::persistence araya::attachment araya::session-query araya::logger ZLIB::ZLIB
    TEST tests/export_test.cpp
)
