# The public header exposes session types (session_header, the event
# vocabulary): the format dependency, not the store. Linking the session
# plugin supplies both its headers and the engine.
araya_add_plugin(persistence
    SOURCES src/jsonl.cpp src/plugin.cpp
    DEPS araya::session
    TEST tests/persistence_test.cpp
)
