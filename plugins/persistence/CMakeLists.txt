# The public header exposes session types (session_header, the event
# vocabulary): the format dependency, not the store - but the format
# headers ship with the session plugin, so the link provides them.
araya_add_plugin(persistence
    SOURCES src/jsonl.cpp src/plugin.cpp
    DEPS araya::araya araya::session
    TEST tests/persistence_test.cpp
    TEST_DEPS araya::session
)
